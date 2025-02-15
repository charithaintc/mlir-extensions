//===- XeTileOpConversion.h - XeTileToXeGPU conversion  -------*- C++ -*-===//
//
// Copyright 2022 Intel Corporation
// Part of the IMEX Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements ConversionPatterns for XeTileOps, used in XeTileToXeGPU
/// conversion, converting the XeTile dialect to the XeGPU dialect.
///
//===----------------------------------------------------------------------===//

#include <imex/Conversion/XeTileToXeGPU/XeTileToXeGPU.h>

#include "ArithOpConversion.h"
#include "SCFOpConversion.h"
#include "XeTileOpConversion.h"
#include "imex/Utils/XeArch.h"
#include "mlir/IR/BuiltinAttributes.h"

namespace imex {

using mlir::vector::ExtractOp;
using mlir::vector::ExtractStridedSliceOp;
using mlir::vector::ShapeCastOp;
using mlir::vector::ShuffleOp;

using VectorTypedValue = mlir::TypedValue<mlir::VectorType>;

// Combine vectors vertically while keeping the logical data layout.
// As an example, given two vectors (2x4xf16) p and q, it will merge
// them in to a 4x4xf16 vector.
//  p1, p2, p3, p4            p1, p2, p3, p4
//  p5, p6, p7, p8            p5, p6, p7, p8
//                     ==>    q1, q2, q3, q4
//  q1, q2, q3, q4            q5, q6, q7, q8
//  q5, q6, q7, q8
static VectorTypedValue stack(mlir::Value v1, mlir::Value v2,
                              mlir::Location loc,
                              mlir::PatternRewriter &rewriter) {
  // LLVM requires operands of a shuffle op has the same type.
  assert(v1.getType() == v2.getType() &&
         "Operands of shuffle should have the same type.");
  auto vecTy = llvm::cast<mlir::VectorType>(v1.getType());
  assert(vecTy.getRank() == 2 && "only supports 2D vectors.");
  auto shape = vecTy.getShape();
  llvm::SmallVector<int64_t> mask(shape[0] + shape[0]);
  std::iota(mask.begin(), mask.end(), 0);
  auto op = rewriter.create<ShuffleOp>(loc, v1, v2, mask);
  return op;
}

// generate linearized shuffle mask for concat.
static llvm::SmallVector<int64_t> getMask(llvm::ArrayRef<int64_t> shape1,
                                          llvm::ArrayRef<int64_t> shape2) {
  assert(shape1.size() == 2 && shape2.size() == 2 && shape1[0] == shape2[0] &&
         "shapes should be 2D and have the same size in dim 0.");
  int64_t size1 = shape1[0] * shape1[1];
  int64_t size2 = shape2[0] * shape2[1];
  llvm::SmallVector<int64_t> mask(size1 + size2);
  for (int64_t i = 0; i < shape1[0]; i++) {
    int64_t s = i * (shape1[1] + shape2[1]);
    int64_t m = s + shape1[1];
    int64_t e = m + shape2[1];
    int64_t v1 = i * shape1[1];
    int64_t v2 = size1 + i * shape2[1];
    std::iota(mask.begin() + s, mask.begin() + m, v1);
    std::iota(mask.begin() + m, mask.begin() + e, v2);
  }
  return mask;
}

// merge vectors horizontally while keep the logical data layout.
// 1 2 3 4   +    10 11 12   =   1 2 3 4 10 11 12
// 5 6 7 8        13 14 15       5 6 7 8 13 14 15
// since there is no direct op in mlir exists, we will
// using ShapeCast and Shuffle to mimic it. It comes with
// cost of complex shuffle masks. the mask for the above one
// will be like this: 0 1 2 3  8  9 10
//                    4 5 6 7 11 12 13
static VectorTypedValue concat(mlir::Value v1, mlir::Value v2,
                               mlir::Location loc,
                               mlir::PatternRewriter &rewriter) {
  // LLVM requires operands of shuffle op has the same type
  auto vecTy = llvm::cast<mlir::VectorType>(v1.getType());
  assert(v1.getType() == v2.getType() &&
         "Operands doesn't have the same type!");
  assert(vecTy.getRank() == 2 && "Currently concat only works on 2D vector.");
  auto size = vecTy.getNumElements();
  auto shape = vecTy.getShape();
  auto elemTy = vecTy.getElementType();
  auto flatTy = mlir::VectorType::get({size}, elemTy);
  auto cast1 = rewriter.create<ShapeCastOp>(loc, flatTy, v1);
  auto cast2 = rewriter.create<ShapeCastOp>(loc, flatTy, v2);
  auto mask = getMask(shape, shape);
  auto shuffleOp = rewriter.create<ShuffleOp>(loc, cast1, cast2, mask);
  auto targetTy =
      mlir::VectorType::get({shape[0], shape[1] + shape[1]}, elemTy);
  auto newOp = rewriter.create<ShapeCastOp>(loc, targetTy, shuffleOp);
  return newOp;
}

// It lowers a pair of Unpack and Pack operators at a time.
// the pattern first matchs TileUnpackOp, and finds its TilePackOp
// user. It can avoid some vector shuffle and extract ops by
// looking at the target block size (innerBlock from TilePackOp)
// directly. It requires 1-1 mapping of UnpackOp and PackOp, which
// should be enforced by a separate pass.
class SgTileUnpackPackOpPattern
    : public SgXeTileToXeGPUConversion<xetile::TileUnpackOp> {
  using SgXeTileToXeGPUConversion<
      xetile::TileUnpackOp>::SgXeTileToXeGPUConversion;

  mlir::LogicalResult
  matchAndRewrite(xetile::TileUnpackOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {

    using funcTy = VectorTypedValue(mlir::Value, mlir::Value, mlir::Location,
                                    mlir::PatternRewriter &);

    auto transform = [&](mlir::ValueRange ins, std::function<funcTy> merge) {
      llvm::SmallVector<mlir::Value> shuffleOps(ins.begin(), ins.end());
      while (shuffleOps.size() > 1) {
        auto curr = shuffleOps;
        assert(curr.size() % 2 == 0 && "The size should be divisible by 2.");
        shuffleOps.clear();
        for (size_t i = 0; i + 1 < curr.size(); i += 2) {
          auto newOp = merge(curr[i], curr[i + 1], op.getLoc(), rewriter);
          shuffleOps.push_back(newOp);
        }
      }
      return shuffleOps[0];
    };

    auto packOp = llvm::dyn_cast<xetile::TilePackOp>(*(op->user_begin()));
    if (!op->hasOneUse() || !packOp)
      return op->emitOpError("[Uexpected Code Pattern]: ")
             << "Upack/Pack should appear as pairs. "
             << "And unpack can be only used by pack. "
             << "Duplicate unpack if necessarry.\n";

    auto inTy = op.getInVec().getType();
    auto outTy = packOp.getOutVec().getType();
    auto inGrids = inTy.getShape().take_front(2);
    auto outGrids = outTy.getShape().take_front(2);
    auto inBlkSizes = op.getInnerBlocksAttr();
    auto outBlkSizes = packOp.getInnerBlocksAttr();
    auto inputs = adaptor.getInVec();

    // specific attention needed for vectors in vnni format,
    // which is applied to load for dpas.
    auto loadOp = op.getInVec().getDefiningOp<xetile::LoadTileOp>();
    bool isVnniFormat = loadOp && (isForDPASA(loadOp) || isForDPASB(loadOp));

    rewriter.setInsertionPoint(op);

    // handle based on the dim0, and save results into intermediates
    llvm::SmallVector<mlir::Value> intermediates;
    if (inBlkSizes[0] == outBlkSizes[0]) { // do nothing
      intermediates = inputs;
    } else if (inBlkSizes[0] < outBlkSizes[0]) { // stack on dim 0
      // `nums` small vectors will be stacked into one big vector
      auto nums = inGrids[0] / outGrids[0];
      llvm::SmallVector<mlir::Value> valSet;
      for (auto j = 0; j < inGrids[1]; j++) {
        for (auto i = 0; i < inGrids[0]; i++) {
          if (i && i % nums == 0) {
            auto newOp = transform(valSet, stack);
            intermediates.push_back(newOp);
            valSet.clear();
          }
          auto idx = i * inGrids[1] + j;
          valSet.push_back(inputs[idx]);
        }
      }

      for (auto i = 0; i < inGrids[0]; i += nums) {
        for (auto j = 0; j < inGrids[1]; j++) {
          llvm::SmallVector<mlir::Value> values;
          for (auto k = 0; k < nums; k++) {
            auto idx = (i + k) * inGrids[1] + j;
            values.push_back(inputs[idx]);
          }
          auto newOp = transform(values, stack);
          intermediates.push_back(newOp);
        }
      }
    } else { // do extract on dim0 using vector::ExtractStridedSliceOp
      intermediates.resize(outGrids[0] * inGrids[1]);
      llvm::SmallVector<int64_t> blkSizes({outBlkSizes[0], inBlkSizes[1]});
      // if the vnni transform applied, vector shape
      // and offset need to be adjusted accordingly.
      if (isVnniFormat) {
        auto vnniAxis = isForDPASB(loadOp) ? 0 : 1;
        auto factor =
            inputs.front().getType().cast<mlir::VectorType>().getShape().back();
        blkSizes[vnniAxis] /= factor;
      }
      // each vector will be horizonally cut into `nums` subvectors
      auto nums = outGrids[0] / inGrids[0];
      llvm::SmallVector<int64_t> strides({1, 1});
      for (auto i = 0; i < inGrids[0]; i++) {
        for (auto j = 0; j < inGrids[1]; j++) {
          auto startPos = i * nums * inGrids[1] + j;
          auto v = inputs[i * inGrids[1] + j];
          for (auto k = 0; k < nums; k++) {
            llvm::SmallVector<int64_t> offsets({k * blkSizes[0], 0});
            auto newOp = rewriter.create<ExtractStridedSliceOp>(
                op.getLoc(), v, offsets, blkSizes, strides);
            auto idx = startPos + k * inGrids[1];
            intermediates[idx] = newOp;
          }
        }
      }
    }

    // handle intermediates based on the dim1, and save results into newOps
    llvm::SmallVector<mlir::Value> newOps;
    llvm::SmallVector<int64_t> interGrids = {outGrids[0], inGrids[1]};

    if (inBlkSizes[1] == outBlkSizes[1]) {
      // do nothing since they have the same size
      newOps = intermediates;
    } else if (inBlkSizes[1] < outBlkSizes[1]) {
      // doing concat since blkSZ of input vector is smaller
      if (loadOp && (isForDPASA(loadOp) || isForDPASB(loadOp))) {
        return op->emitOpError("[Unexpected rare case]: ")
               << "It rarly happens that we need to do concat on vnni "
               << "transformed vectors (which is 3D instead of 2D). "
               << "It is essentially a stack on the 2nd dim, but it is "
               << "not implemented yet.\n";
      }
      // `nums` of small vectors will be concated into a big one
      size_t nums = inGrids[1] / outGrids[1];
      llvm::SmallVector<mlir::Value> valSet;
      for (auto i = 0; i < interGrids[0]; i++) {
        for (auto j = 0; j < interGrids[1]; j++) {
          valSet.push_back(intermediates[i * interGrids[1] + j]);
          if (valSet.size() == nums) {
            auto newOp = transform(valSet, concat);
            newOps.push_back(newOp);
            valSet.clear();
          }
        }
      }
    } else { // doing extract on dim 1
      llvm::SmallVector<int64_t> blkSizes({outBlkSizes[0], outBlkSizes[1]});
      // if vnni transform applied, vector shape
      // and offset needs to adjusted accordingly.
      if (isVnniFormat) {
        auto vnniAxis = isForDPASB(loadOp) ? 0 : 1;
        auto factor =
            inputs.front().getType().cast<mlir::VectorType>().getShape().back();
        blkSizes[vnniAxis] /= factor;
      }
      llvm::SmallVector<int64_t> strides({1, 1});
      auto nums = outGrids[1] / interGrids[1];
      for (auto i = 0; i < interGrids[0]; i++) {
        for (auto j = 0; j < interGrids[1]; j++) {
          auto v = intermediates[i * interGrids[1] + j];
          for (int64_t k = 0; k < nums; k++) {
            llvm::SmallVector<int64_t> offsets({0, k * blkSizes[1]});
            auto newOp = rewriter.create<ExtractStridedSliceOp>(
                op.getLoc(), v, offsets, blkSizes, strides);
            newOps.push_back(newOp);
          }
        }
      }
    }

    rewriter.replaceOp(packOp, newOps);
    rewriter.eraseOp(op);
    return mlir::success();
  }
};

int getBlockArrayLength(mlir::Type elemTy, int block_width) {
  return 64 * 8 / elemTy.getIntOrFloatBitWidth() / block_width;
}

// It rewrites a XeTile::init_tile into one or more XeGPU::create_nd_desc
// It is one of start points of generating 1:N values.
class SgInitTileOpPattern
    : public SgXeTileToXeGPUConversion<xetile::InitTileOp> {
  using SgXeTileToXeGPUConversion<
      xetile::InitTileOp>::SgXeTileToXeGPUConversion;

  mlir::LogicalResult
  matchAndRewrite(xetile::InitTileOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto source = op.getSource();
    auto tileTy = op.getType();
    auto innerBlk = tileTy.getInnerBlocks();
    auto shape = tileTy.getShape();
    auto indexType = rewriter.getIndexType();

    if (tileTy.getRank() != 2)
      return op.emitOpError("The tile shape should be 2D.");

    if (!innerBlk || innerBlk.size() != 2)
      return op.emitOpError("Missing valid innerBlock for the tile in op.");

    // using array_length for load if dim1 of innerBlocks
    // is smaller than dim 1 of shape.
    auto array_length =
        isForLoad(op) && shape[1] > innerBlk[1]
            ? getBlockArrayLength(tileTy.getElementType(), innerBlk[1])
            : 1;

    auto width = array_length * innerBlk[1];

    llvm::SmallVector<int64_t, 2> blocks(
        {shape[0] / innerBlk[0], shape[1] / width});

    llvm::SmallVector<mlir::Value> offsets;
    auto staticOffsets = op.getStaticOffsets();
    auto dynamicOffsets = op.getOffsets();
    for (size_t i = 0, j = 0; i != staticOffsets.size(); i++) {
      if (mlir::ShapedType::isDynamic(staticOffsets[i])) {
        offsets.push_back(dynamicOffsets[j++]);
      } else {
        offsets.push_back(rewriter.create<mlir::arith::ConstantOp>(
            op.getLoc(), rewriter.getIndexAttr(staticOffsets[i])));
      }
    }

    auto offsetsX = offsets[0];
    auto offsetsY = offsets[1];
    auto tDescTy = xegpu::TensorDescType::get(
        innerBlk, tileTy.getElementType(), xegpu::MemoryScope::GLOBAL,
        array_length, true /*boundary_check*/, {} /*scattered*/,
        {} /*mapping*/);

    auto createIndexConstant = [&](mlir::Type type, int64_t value) {
      auto attr = rewriter.getIndexAttr(value);
      return rewriter.create<mlir::arith::ConstantOp>(loc, type, attr);
    };

    rewriter.setInsertionPoint(op);

    llvm::SmallVector<mlir::Value> xegpuOps;
    for (int i = 0; i < blocks[0]; i++) {
      for (int j = 0; j < blocks[1]; j++) {
        auto subOffX = createIndexConstant(indexType, (innerBlk[0] * i));
        auto subOffY = createIndexConstant(indexType, (width * j));
        auto tDescOffsetX =
            rewriter.createOrFold<mlir::arith::AddIOp>(loc, subOffX, offsetsX);
        auto tDescOffsetY =
            rewriter.createOrFold<mlir::arith::AddIOp>(loc, subOffY, offsetsY);
        mlir::SmallVector<mlir::OpFoldResult> tDescOffsets{tDescOffsetX,
                                                           tDescOffsetY};

        // TODO: this needs improvement, it assumes the source is static
        // memeref.
        auto createNdOp = rewriter.create<xegpu::CreateNdDescOp>(
            op.getLoc(), tDescTy /*resultTy*/, source /*source*/,
            tDescOffsets /*offsets*/, imex::xegpu::Mode::VC /*mode*/);

        xegpuOps.push_back(createNdOp);
      }
    }

    rewriter.replaceOp(op, xegpuOps);
    return mlir::success();
  }
};

// It lowers a XeTile::prefetch_tile into one or more XeGPU::prefetch_2d.
// The adaptor will provide the set of xegpu.create_nd_desc lowered for
// its input tile.
struct SgPrefetchTileOpPattern
    : public SgXeTileToXeGPUConversion<xetile::PrefetchTileOp> {
  using SgXeTileToXeGPUConversion<
      xetile::PrefetchTileOp>::SgXeTileToXeGPUConversion;

  ::mlir::LogicalResult
  matchAndRewrite(xetile::PrefetchTileOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {
    auto tileTy = op.getTile().getType();
    auto tiles = adaptor.getTile();
    if (tileTy.getRank() != 4)
      return mlir::failure();
    auto shape = tileTy.getShape();

    if (shape[0] * shape[1] != (int64_t)tiles.size()) {
      op.emitOpError("Failed to lower LoadTileOp because shape[0] * shape[1] "
                     "!= sources.size().");
      return mlir::failure();
    }

    auto L1 = xegpu::CacheReadHintAttr::get(op.getContext(),
                                            xegpu::CacheReadHint::CACHED);
    auto L2 = xegpu::CacheReadHintAttr::get(op.getContext(),
                                            xegpu::CacheReadHint::CACHED);
    auto L3 = xegpu::CacheReadHintAttr::get(op.getContext(),
                                            xegpu::CacheReadHint::CACHED);

    for (int i = 0; i < shape[0]; i++) {
      for (int j = 0; j < shape[1]; j++) {
        auto tile = tiles[i * shape[1] + j];
        rewriter.create<xegpu::PrefetchNDOp>(op.getLoc(), tile, L1, L2, L3,
                                             imex::xegpu::Mode::VC);
      }
    }

    rewriter.eraseOp(op);

    return mlir::success();
  }
};

// It lowers XeTile::load_tile into one or more XeGPU::load_2d
// The adaptor will provide the set of xegpu.create_nd_desc lowered for
// its input tile.
struct SgLoadTileOpPattern
    : public SgXeTileToXeGPUConversion<xetile::LoadTileOp> {
  using SgXeTileToXeGPUConversion<
      xetile::LoadTileOp>::SgXeTileToXeGPUConversion;

  mlir::LogicalResult
  matchAndRewrite(xetile::LoadTileOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {
    auto tileTy = op.getSource().getType();
    auto blockSZ = tileTy.getInnerBlocks();

    // It expects the tile has been tiled using blocking pass
    if (!blockSZ)
      return mlir::failure();

    auto elemTy = tileTy.getElementType();
    auto sources = adaptor.getSource();

    auto ctx = op.getContext();
    auto L1 = xegpu::CacheReadHintAttr::get(ctx, xegpu::CacheReadHint::CACHED);
    auto L2 = xegpu::CacheReadHintAttr::get(ctx, xegpu::CacheReadHint::CACHED);
    auto L3 = xegpu::CacheReadHintAttr::get(ctx, xegpu::CacheReadHint::CACHED);

    mlir::IntegerAttr vnniAttr;
    // TODO: move these two into architecture abstracture in future.
    const int SIMD_WIDTH_IN_BITS = 32;
    int factor = SIMD_WIDTH_IN_BITS / elemTy.getIntOrFloatBitWidth();
    if ((isForDPASA(op) || isForDPASB(op)) && factor > 1) {
      // vnni transform needed if they are used in mma and elemTy bits < 32
      int axis = isForDPASB(op) ? 0 : 1;
      vnniAttr = rewriter.getI32IntegerAttr(axis);
    }

    // TODO: add transpose info
    mlir::DenseI64ArrayAttr transposeAttr;

    rewriter.setInsertionPoint(op);
    llvm::SmallVector<::mlir::Value> xegpuOps;
    for (auto src : sources) {
      auto tdescTy = llvm::dyn_cast<xegpu::TensorDescType>(src.getType());
      assert(tdescTy && "Expecting a TensorDescType value for load_tile.");
      auto shape = tdescTy.getShape().vec();
      auto array_length = tdescTy.getArrayLength();

      if (vnniAttr) {
        auto axis = vnniAttr.getInt();
        shape[axis] /= factor;
        shape.push_back(factor);
      }

      if (array_length != 1)
        shape.insert(shape.begin(), array_length);

      auto vectorTy = mlir::VectorType::get(shape, tileTy.getElementType());
      auto ldOp = rewriter.create<xegpu::LoadNDOp>(
          op.getLoc(), vectorTy, src, vnniAttr, transposeAttr, L1, L2, L3,
          imex::xegpu::Mode::VC);
      if (array_length == 1) {
        xegpuOps.push_back(ldOp);
      } else {
        for (auto i = 0; i < array_length; i++) {
          auto extractOp = rewriter.create<ExtractOp>(op.getLoc(), ldOp, i);
          xegpuOps.push_back(extractOp);
        }
      }
    }

    rewriter.replaceOp(op, xegpuOps);
    return mlir::success();
  }
};

// It lowers a XeTile::store_tile into one or more XeGPU::store_2d
// The adaptor will provide the set of xegpu.create_nd_desc lowered for
// its input tile, and similar to its input vector value.
struct SgStoreTileOpPattern
    : public SgXeTileToXeGPUConversion<xetile::StoreTileOp> {
  using SgXeTileToXeGPUConversion<
      xetile::StoreTileOp>::SgXeTileToXeGPUConversion;

  ::mlir::LogicalResult
  matchAndRewrite(xetile::StoreTileOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {
    auto tiles = adaptor.getTile();
    auto values = adaptor.getValue();

    if (tiles.size() != values.size()) {
      return op.emitOpError("[Failed to lower the StoreOp]")
             << "tile and value size doesn't match."
             << "tiles: " << tiles.size() << ", "
             << "values: " << values.size() << "\n";
    }

    auto context = op.getContext();
    auto WRITE_BACK = xegpu::CacheWriteHint::WRITE_BACK;
    auto L1 = xegpu::CacheWriteHintAttr::get(context, WRITE_BACK);
    auto L2 = xegpu::CacheWriteHintAttr::get(context, WRITE_BACK);
    auto L3 = xegpu::CacheWriteHintAttr::get(context, WRITE_BACK);
    for (size_t i = 0; i < tiles.size(); i++)
      rewriter.create<xegpu::StoreNDOp>(op.getLoc(), tiles[i], values[i], L1,
                                        L2, L3, imex::xegpu::Mode::VC);

    rewriter.eraseOp(op);
    return ::mlir::success();
  }
};

// It lowers a XeTile::tile_mma into one or more XeGPU::dpas
// The adaptor provides new inputs for each old input.
struct SgTileMMAOpPattern
    : public SgXeTileToXeGPUConversion<xetile::TileMMAOp> {
  using SgXeTileToXeGPUConversion<xetile::TileMMAOp>::SgXeTileToXeGPUConversion;

  ::mlir::LogicalResult
  matchAndRewrite(xetile::TileMMAOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {

    auto aShape = op.getAType().getShape();
    auto bShape = op.getBType().getShape();

    if (aShape.size() != 4 || bShape.size() != 4) {
      op.emitOpError() << "Operand A and B for mma should be 4d.\n";
      return mlir::failure();
    }

    if (aShape[3] != bShape[2] || aShape[1] != bShape[0]) {
      op.emitOpError() << "A and B size doesn't match. A should be m x k, and "
                          "B should be k x n";
      return mlir::failure();
    }

    uint64_t M = aShape[0];
    uint64_t K = aShape[1];
    uint64_t N = bShape[1];

    auto loc = op.getLoc();
    auto AValues = adaptor.getA();
    auto BValues = adaptor.getB();
    auto CValues = adaptor.getC();

    auto elemTy = op.getOutput().getType().getElementType();
    auto subCTy = mlir::VectorType::get({aShape[2], bShape[3]}, elemTy);

    mlir::SmallVector<mlir::Value> xegpuOps;
    for (uint64_t i = 0; i < M; i++) {
      for (uint64_t j = 0; j < N; j++) {
        mlir::Value tmpC;
        if (op.getC())
          tmpC = CValues[i * N + j]; // init with acc
        for (uint64_t k = 0; k < K; k++) {
          auto aVec = AValues[i * K + k];
          auto bVec = BValues[k * N + j];
          tmpC = rewriter.create<xegpu::DpasOp>(
              loc, subCTy /*result*/, aVec /*lhs*/, bVec /*rhs*/, tmpC /*acc*/,
              imex::xegpu::Mode::VC);
        }
        xegpuOps.push_back(tmpC);
      }
    }
    rewriter.replaceOp(op, xegpuOps);
    return mlir::success();
  }
};

struct SgUpdateTileOffsetOpPattern
    : public SgXeTileToXeGPUConversion<xetile::UpdateTileOffsetOp> {
  using SgXeTileToXeGPUConversion<
      xetile::UpdateTileOffsetOp>::SgXeTileToXeGPUConversion;

  mlir::LogicalResult
  matchAndRewrite(xetile::UpdateTileOffsetOp op, OpAdaptor adaptor,
                  XeGPUOneToNPatterRewriter &rewriter) const override {
    auto offsetX = op.getOffsetX();
    auto offsetY = op.getOffsetY();
    auto tiles = adaptor.getTile();

    llvm::SmallVector<mlir::Value> newOps;
    for (const auto &tile : tiles) {
      auto xegpuTile = rewriter.create<xegpu::UpdateNDOffsetOp>(
          op.getLoc(), tile.getType(), tile, mlir::ValueRange{offsetX, offsetY},
          imex::xegpu::Mode::VC);
      newOps.push_back(xegpuTile);
    }
    rewriter.replaceOp(op, newOps);
    return mlir::success();
  }
};

void populateXeTileOpConversionPatterns(imex::XeGPUTypeConverter &converter,
                                        mlir::RewritePatternSet &patterns) {
  patterns.insert<SgInitTileOpPattern, SgPrefetchTileOpPattern,
                  SgTileUnpackPackOpPattern, SgLoadTileOpPattern,
                  SgStoreTileOpPattern, SgTileMMAOpPattern,
                  SgUpdateTileOffsetOpPattern>(patterns.getContext(),
                                               converter);
}

} // namespace imex
