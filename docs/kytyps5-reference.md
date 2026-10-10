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

## Borrow-input subtraction

Captured instruction at shader PC 0xB84: 54003080. Upstream has the exact raw word in VectorSubrevCoCiU32ExactRawOnGpu and covers masked-exec behavior nearby in tests/ShaderRecompilerComputeTests.cpp.
Reference implementation: frontend/translate/Control.cpp, Translator::SUBB_U32, dispatched by frontend/translate/Vector.cpp.

Adapted both V_SUB_CO_CI_U32 (0x29) and V_SUBREV_CO_CI_U32 (0x2A), including their VOP3 encodings. Reverse form swaps arithmetic inputs; both consume the lane's VCC bit and write the borrow mask. Result is lhs-rhs-borrow_in; outgoing borrow is (rhs>lhs) OR (borrow_in>partial). The existing two-input subtraction path remains supported. Register collection includes both dwords of the carry/borrow input mask.

Regression covers exact captured decoding, production subtraction emitter, lanes 0/31/32/63, borrow on/off and unsigned wrap boundaries.

### Scalar trap with disabled trap handler
Runtime after the borrow fix reached SOPP 0x12, exact word 0xbf920001 at PC 0x1f80.
Upstream ScalarAluOps.cpp identifies S_TRAP; translate/Scalar.cpp emits ControlNop.
ShaderRecompilerComputeTests.cpp::ScalarTrapDisabledFallsThroughExactRaw verifies that
the identical word continues into the following V_MOV and store (expected 42).
Our explicit STrap decoder preserves the immediate and lowers to existing ControlNop.
This assumes no guest GPU trap handler is installed; it does not implement GPU debugger traps.
Captured decoder regression checks its immediate and explicit trap opcode.
