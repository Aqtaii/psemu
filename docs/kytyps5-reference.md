# KytyPS5 reference and adaptation record

Upstream: https://github.com/KytyPS5/KytyPS5
Inspected commit: 307fdcf7ad5d9d15b5966288fcc598ac9dbca188.
Both repository roots provide GPL-2.0 license text; preserve each imported file's individual copyright/license notices. Existing Kyty MIT notices remain applicable to their original files.

## Unsigned 64-bit greater-than comparison

Captured Astro Bot instruction: shader PC 0x784, words d4e4006a 0001006a.
Upstream identifies VOP3 0xE4 as V_CMP_GT_U64:
- src/graphics/shader/recompiler/frontend/decode/VectorAluOps.cpp
- src/graphics/shader/recompiler/frontend/translate/Vector.cpp
- tests/ShaderRecompilerComputeTests.cpp, VectorCompareInteger64Edges.

Adaptation retains psemu's IR and mask-result emitter. Both dwords of each source register pair are loaded; unsigned high words are compared first, then low words when high words are equal. This preserves the upstream predicate without requiring Vulkan shaderInt64. Decoder, IR mapping, register collection, compare classification and emitter dispatch are updated together.

Regressions cover the captured instruction and production comparison operations across high/low boundaries, signed-bit boundaries and 10000 generated input pairs.

## Next compatibility audit

Upstream uses a newer value-based shader IR. Whole backend replacement has not been reviewed. For later missing instructions compare decoder, frontend translation and tests together, then adapt complete semantic paths.

Prior read-only AGC audit found parameter-count differences requiring NID/ABI validation: DriverSubmitCommandBuffer (3 vs 2), GetIsTrinityMode (0 vs 1 output pointer), QueueEndOfPipeActionPatchData (4 vs 2). The generic VEH bridge accepts 10 integer arguments; CbBranch, CbReleaseMem and DcbDmaData require 12. Default native GOT dispatch bypasses that bridge.

Earlier GDS DMA and append/consume offset fixes were informed by upstream compute tests. Record later adaptations and their validation here; upstream availability does not establish that an untested integration is correct.
