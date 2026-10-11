# Jetsku/KytyPS5 reference audit — 2026-10-11

Pinned source: https://github.com/Jetsku/KytyPS5/tree/f9e19582f0deac0d1226a7ce3faae30a9ab408ab
Local sparse checkout: D:/proje/psemu-codex/work/jetsku-kytyps5-reference
License: GPL-2.0. Preserve attribution for any imported implementation.
Original KytyPS5 reference remains pinned to 307fdcf7ad5d9d15b5966288fcc598ac9dbca188.

## Findings and adoption order

1. Current blocker: DCC metadata ownership and clear consumption.
   src/graphics/host_gpu/renderer/cache/textureCache.cpp::MaterializeDccClear,
   TryMaterializeGpuDccClear, TryMaterializeGpuMetadataClear.
   CPU fallback verifies a whole metadata slice, records ClearImage, and publishes
   expanded 0xff keys only if the clear succeeds. Rejected clears retain the key.
   GPU reuse is tied to exact metadata range, content revision and decodable mask.
   Metadata/color overlap and incompatible image ownership are rejected.
   Port the ownership contract before adopting the GPU optimization.

2. Clear elimination before sampling.
   docs/metadata-clear-eliminate.md explains color-buffer mode 2: it must resolve
   pending metadata even when later sampled descriptors omit metadata.
   tests/ShaderRecompilerComputeTests.cpp::CheckRenderExecutorDccFixedClearFloat,
   CheckRenderExecutorCmaskFastClear, CheckSampledDccClear are relevant regression
   references. Required psemu coverage: clear -> consume key -> later paint/rebind
   preserves painted pixels; rejected clear does not consume the key.
   Native clear foundation currently exists but is not called for Astro DCC.

3. GPU-side metadata inspection after correctness.
   src/graphics/host_gpu/renderer/image/dccClear.{h,cpp};
   src/graphics/host_gpu/shaders/gpu_dcc_validate.comp and gpu_dcc_clear.comp.
   Image and canonical metadata buffer must survive scheduler completion.
   This relies on scheduler/image ownership abstractions psemu does not share;
   copying these files alone is insufficient.

4. Software RT / guest address writes after DCC dispatch.
   docs/EXPERIMENTAL.md int16 records software IMAGE_BVH_INTERSECT_RAY, runtime
   BDA stores/atomics, traversal budgets and shader cache fingerprints.
   Audit these against actual subsequent psemu faults. Do not import zero-on-
   unmapped, skipped shader or null-resource fallbacks as proof of correctness.

5. Performance after a first frame and stable gameplay.
   docs/CHANGES-U59.md, docs/PIPELINE-STUTTER.md and docs/EXPERIMENTAL.md:
   program caches/precompilation, PGO, bounded guest shader arrays, VRAM budgets
   and reduced tracking faults. Performance evidence is hardware/build/scene
   dependent; reported RTX 3090 + Ryzen 7950X3D results are not psemu predictions.
   The fork explicitly does not claim a general 60 FPS result.

No emulator source was imported in this audit. No runtime advancement is claimed.
