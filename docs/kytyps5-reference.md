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

### Astro BVH decoder foundation
Captured MIMG0xe6 at PC0x2190: f1989f07 00040505 4442413d 4543403e 00004746.
Upstream frontend/decode/ImageOps.cpp defines IMAGE_BVH_INTERSECT_RAY (0xe6/0xe7):
11/12 full-float ray DWORDs, R128 descriptor and DMASK0xf; NSA length must be0 or3.
Tests preserve v5, s[16:19], all10 nonsequential ray registers, wide/sequential encoding,
reject A16/D16, missing R128, partial DMASK, incomplete/truncated NSA.
Decoder support alone is NOT runnable BVH support. Upstream translator Memory.cpp,
spirvEmitterBvh.cpp, ResourceTracking.cpp and ShaderInfoCollection.cpp also participate.
Our backend has no physical storage buffer/BDA guest-address translation path.
Next: bind the real BVH backing memory or implement guest-address GPU lookup; adapt
triangle and full/half-box kernels and match upstream GPU conformance fixtures.
Keep unsupported lowering explicit until real intersection execution is implemented.

### BVH memory integration audit
Upstream fixture: tests/ShaderRayTracingGpuTests.inc::BvhIntersections
(included by ShaderRecompilerComputeTests.cpp), pinned commit as above.
It builds the four-word descriptor in shader SGPRs rather than materializing
an image descriptor on the host. Preserve those live values in the IR.
Required cases: triangle kinds 0..3, FP16 boxes kind4, FP32 boxes kind5,
invalid kind/address/ray, barycentric versus triangle-id outputs, sorted
versus original-order children, exclusive ray extent, box growth, inactive
EXEC destinations, NSA, and wide node pointers above the 32-bit range.
The FP32 fixture also deliberately splits a 128-byte node across guest pages
whose physical backing is separated by a 64-byte gap. Resolve the second
64-byte block independently; never assume adjacent guest pages are adjacent
in physical GPU backing.

Local constraints: ResourceMaterialization.cpp specializes known FLAT bases
into FlatAddressWindowSize (0x10000), and renderer/descriptors.cpp::NativeAddressBuffer
binds one contiguous read-only host range. The normal buffer/image descriptor
formats do not represent a BVH descriptor. Extending that window alone would
not handle shader-produced descriptors or discontiguous page backing.

Integration order:
1. Add dedicated BvhIntersect IR carrying four live descriptor words, all
   11/12 ray words, node width and EXEC; keep ordinary images separate.
2. Provide command-lifetime guest-page-to-GPU-buffer address lookup, with
   independent node block resolution, bounds and missing-page behavior.
3. Adapt upstream triangle/box SPIR-V routines to this IR and memory interface.
4. Run the above conformance cases and SPIR-V validation before installing
   a runtime build; then rerun Astro and record the next actual blocker.
No constant-miss or null-output substitute constitutes intersection support.

### BVH IR integration
Dedicated BvhIntersect IR now stores four live SGPR descriptor sources and
11/12 explicit ray operands in one instruction. Destination is a four-VGPR
result; all inputs must be loaded before masked writes when registers overlap.
Register collection includes every NSA ray operand and all four output VGPRs;
the standard EXEC mask registers remain collected. Ordinary image resource
tracking is bypassed. Existing scalar-source provenance tracks the four SGPRs.
Production lowering tests cover captured raw decoding, NSA/sequential/wide
forms, descriptor words, overlap, copying/equality, malformed register ranges
and failure without partially appended IR. GPU EXEC/write semantics remain
unverified until intersection emission is implemented.
Emitter contract explicitly rejects BVH execution until GPU guest-address
mapping and real intersection emission are available.

### BVH physical-address prerequisites
KytyPS5 presentation/window/vulkanWindow.cpp requires bufferDeviceAddress and
shaderInt64. psemu now queries both and enables them together only if available.
VMA receives its buffer-device-address allocator flag; storage buffers get
shader-device-address usage and a queried device address, cleared on deletion.
No hardware ray tracing extension is required by the software intersection path.
This is allocation infrastructure only: the guest GPU page table, descriptor
bindings, fault resolution and intersection emission remain to be integrated.
Unsupported hardware retains the ordinary buffer allocation path; BVH execution
still fails explicitly rather than emitting an invalid physical-address shader.

### Astro scalar first-set-bit after BVH decode
Actual e0ea78f run progressed beyond the former MIMG0xe6 decode rejection,
then rejected SOP1 0x14 at PC0x2280, exact word0xbeeb146a.
Upstream ScalarAluOps.cpp identifies S_FF1_I32_B64; translate/Integer.cpp::S_FF_I32_B64
reads both source halves before writing the result and preserves SCC.
ShaderRecompilerComputeTests.cpp::ScalarBitfieldPack includes expected first-set
positions 40 and5, and all-zero output0xffffffff.
Added dedicated FindLsbU64 IR/emission using two FindILsb32 operations: prefer
nonzero low half, otherwise high position+32, otherwise0xffffffff. Captured
instruction reads VCC_LO/HI and overwrites VCC_HI; register collection includes
both input halves and emission reads them before its single destination write.
Tests compile the actual emitter with a SPIR-V operation evaluator, covering all
64 bit positions, zero/all-ones, overlap ordering and10000 generated inputs.
The new runtime test has not yet established BVH lowering/execution success;
the previous run stopped in CFG decoding before those phases.

## Astro descriptor provenance capture (464e1fe)

Reference remains KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188, src/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp::Collect: unresolved scalar loads use the restricted IndirectBuffer/DMA path. No fallback is enabled in psemu yet.

The failed compute shader previously exited before its original/IR dump could be saved. Preserve decoded and CFG/IR diagnostics on resource-tracking failure and write failed CS dumps before the fatal exit. This work adds diagnostics only; successful shaders keep their existing dump policy.

Windows CI run 38078584242 passed every regression and complete build. Verified archive digest plus all 19 manifest entries. Separate Astro test (PID 17792, now exited) reproduced PC 0x20b8 with shader address 0x265a0531000. Captured 14384-byte original, 114737-byte RDNA2 text, and 319794-byte CFG/IR under D:/proje/psemu/_Shaders/original.

Evidence: PC 0x2090 s_lshl_b64 s8,s28,3 feeds PC 0x20b8 s_load_dwordx4 s20,s8,offset=88. ScalarProvenance::Operation does not model ShiftLeftLogicalU64, so it loses this address. s28/s29 are also overwritten by v_readlane within the traversal loop (0x2294, 0x22dc, 0x22ec and subsequent paths); CPU provenance support alone cannot be assumed to resolve all iterations. Follow up with pair-shift provenance tests and the restricted GPU scalar address-load path. Do not replace unknown addresses with zero, and do not claim BVH execution or first frame.

## Scalar 64-bit address shift provenance (ede938f)

Compared KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188, src/graphics/shader/recompiler/frontend/translate/Scalar.cpp S_LSHL_B64/S_LSHR_B64 dispatch to 64-bit integer shift operations. Adapted the psemu CPU provenance graph to represent both result halves, each with the original low/high source and shift count. SrtWalker evaluates a uint64 shift with a 6-bit masked count. This changes analysis only; existing GPU shift emission remains intact.

Regression tests/scalar_address_shift_test.cpp compiles real ScalarProvenance.cpp and SrtWalker.cpp, checks both directions, counts 0..127, boundary/carry patterns, disjoint/in-place/partially overlapping destinations, and unknown inputs remaining unresolved. CI step Test scalar 64-bit address shift provenance passed in run 38091137490. Full build and runtime result pending below.

Full CI run 38091137490 completed successfully; archive digest and all 19 manifest files verified. Separate runtime PID19332 exited at PC0x2030 s_load_dwordx16 s16,s8,offset=0, unresolved source28 dword0. Error now retains the phi/shift/add reaching-definition chain instead of collapsing the shifted address to unknown at 0x20b8. This is improved provenance, not boot progress. The dynamic address comes through VCC pair shift at 0x2020 and add/add-carry at 0x2024/0x202c. Failed raw shader and IR captured for address 0x1b1a66b1000 under _Shaders/original. Next integration must support scalar runtime addresses, including x16 split into DWORD operations with the pre-load base preserved when destinations overlap address registers; unknown CPU descriptors must not become fake static SRT reads. BVH emission remains absent.

## Sparse GPU address snapshot foundation

Reference KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188: src/graphics/host_gpu/cache/bufferCache.cpp::ChangeRegister and src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp::DefineGetBdaPointer (verify local paths when following up). The original implementation maintains page-to-BDA mapping; psemu uses sorted resident cache-owner ranges because full 48-bit dense mapping is unsuitable for its dynamic Windows guest addresses. No upstream source copied in this change.

New guestAddressTable.h defines a 32-byte SSBO entry (guest base, device base, size, reserved). Build is bounded to 65536 ranges, sorted, non-overlapping, validates guest/device arithmetic and publishes transactionally. Resolve requires the complete block to stay inside one owner; discontiguous backing must be resolved independently. Missing ranges fail explicitly.

BufferCache::CaptureAddressSnapshot holds resource/cache locks in existing order and captures shared backing ownership, including device addresses. Snapshots do not synchronize CPU-dirty data or populate absent pages. Callers must prepare coherence and retain snapshots through fences before eventual dispatch integration. This API is not yet called by shader execution; no dynamic scalar fallback enabled.

Regression tests/guest_address_table_test.cpp covers captured high guest-address shape, independently resolved 64-byte blocks around a 16KB boundary, unrelated device backing, overlap/overflow/missing ranges, null arguments, maximum entry count, and transactional invalid-input preservation.
