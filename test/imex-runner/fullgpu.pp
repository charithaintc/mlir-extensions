builtin.module(
    add-gpu-regions,
    ndarray-dist,
    func.func(dist-coalesce),
    func.func(dist-infer-elementwise-cores),
    convert-dist-to-standard,
    canonicalize,
    overlap-comm-and-compute,
    add-comm-cache-keys,
    lower-distruntime-to-idtr,
    convert-ndarray-to-linalg,
    canonicalize,
    func.func(tosa-make-broadcastable),
    func.func(tosa-to-linalg),
    func.func(tosa-to-tensor),
    canonicalize,
    linalg-fuse-elementwise-ops,
    arith-expand,
    memref-expand,
    func.func(empty-tensor-to-alloc-tensor),
    one-shot-bufferize{unknown-type-conversion=identity-layout-map function-boundary-type-conversion=identity-layout-map bufferize-function-boundaries}
    canonicalize,
    imex-remove-temporaries,
    func.func(convert-linalg-to-parallel-loops),
    func.func(scf-parallel-loop-fusion),
    // GPU
    func.func(imex-add-outer-parallel-loop),
    func.func(gpu-map-parallel-loops),
    func.func(convert-parallel-loops-to-gpu),
    // insert-gpu-allocs pass can have client-api = opencl or vulkan args
    func.func(insert-gpu-allocs{in-regions=1}),
    drop-regions,
    canonicalize,
    normalize-memrefs,
    // Unstride memrefs does not seem to be needed.
    //  func.func(unstride-memrefs),
    func.func(lower-affine),
    gpu-kernel-outlining,
    canonicalize,
    cse,
    // The following set-spirv-* passes can have client-api = opencl or vulkan
    // args
    set-spirv-capabilities{client-api=opencl},
    gpu.module(set-spirv-abi-attrs{client-api=opencl}),
    canonicalize,
    fold-memref-alias-ops,
    imex-convert-gpu-to-spirv,
    spirv.module(spirv-lower-abi-attrs),
    spirv.module(spirv-update-vce),
    // func.func(llvm-request-c-wrappers),
    serialize-spirv,
    expand-strided-metadata,
    lower-affine,
    convert-gpu-to-gpux,
    convert-func-to-llvm,
    convert-math-to-llvm,
    convert-gpux-to-llvm,
    finalize-memref-to-llvm,
    reconcile-unrealized-casts
)
