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

Reference KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188: src/graphics/host_gpu/renderer/cache/bufferCache.cpp::ChangeRegister and src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp::DefineGetBdaPointer. The original implementation maintains page-to-BDA mapping; psemu uses sorted resident cache-owner ranges because full 48-bit dense mapping is unsuitable for its dynamic Windows guest addresses. No upstream source copied in this change.

New guestAddressTable.h defines a 32-byte SSBO entry (guest base, device base, size, reserved). Build is bounded to 65536 ranges, sorted, non-overlapping, validates guest/device arithmetic and publishes transactionally. Resolve requires the complete block to stay inside one owner; discontiguous backing must be resolved independently. Missing ranges fail explicitly.

BufferCache::CaptureAddressSnapshot holds resource/cache locks in existing order and captures shared backing ownership, including device addresses. Snapshots do not synchronize CPU-dirty data or populate absent pages. Callers must prepare coherence and retain snapshots through fences before eventual dispatch integration. This API is not yet called by shader execution; no dynamic scalar fallback enabled.

Regression tests/guest_address_table_test.cpp covers captured high guest-address shape, independently resolved 64-byte blocks around a 16KB boundary, unrelated device backing, overlap/overflow/missing ranges, null arguments, maximum entry count, and transactional invalid-input preservation.

Validation: source commit 46d4d300bd4ce5c72d24160fbb924fcbeeb0e7cb, CI38092001848 all regressions and full Windows build succeeded. Downloaded outputs/psemu-windows-x64-astro-address-table.zip and extracted work/ci-astro-address-table under D:/proje/psemu-codex. Archive SHA256 digest plus all19 manifest entries verified. No runtime test because CaptureAddressSnapshot is not wired into dispatch yet; previous Astro dynamic scalar load blocker is unchanged. Next: table binding/physical SPIR-V lookup, coherence preparation/fence retention, then dynamic scalar reads preserving group base and missing-page diagnostics.

## Shader address-table binding and lookup

Compared pinned KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188, src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp::DefineGetBdaPointer and src/graphics/host_gpu/renderer/cache/bufferCache.cpp::ChangeRegister. Adaptation is original code for psemu sorted range snapshots, not a direct copy of the upstream dense-page implementation.

GuestAddressTable descriptor is declared only for a program explicitly requesting it (default disabled). Binding allocation, SPIR-V declaration/topology checks and host descriptor writes use the same kind. DefineGuestAddressLookup emits a callable uvec2-address function over the serialized 8-DWORD entries using 32-bit pair arithmetic and bounded upper-bound search (17 decisions for 65536 entries). Missing, empty, malformed-length and cross-owner reads return zero. This is address resolution only: physical-storage loads and fault reporting remain follow-up work.

Host binding uploads exact serialized bytes (OpArrayLength excludes stream allocation padding), keeps every backing buffer until the command fence, and checks recording reset after upload. Unprepared CPU/GPU-dirty or image-alias ranges are rejected, not silently made coherent. Dynamic execution remains disabled until coherence preparation, missing-page reporting and scalar group address capture are integrated.

Test guest_address_lookup_test.cpp interprets emitted production SPIR-V operations and compares to the separate CPU range resolver for maximum-size tables, boundary cases and 2000 random accesses. It also checks the production binding allocator and builds a callable compute module for spirv-val --target-env vulkan1.1. Validation result recorded after CI.

Validation: 6b58cdc40a734915c2afbfd23ed5e58f8eaa7fd5, CI38093115263 successful across all regressions, lookup interpretation, Vulkan1.1 spirv-val and complete Windows build. Artifact archive digest and19 manifest files verified in work/ci-astro-address-lookup (D:/proje/psemu-codex). No Astro runtime test: dynamic address execution flag remains default false, so the previous scalar-load blocker is unchanged. Follow up with actual physical-storage read emission, missing-backing diagnostics/population, coherence preparation and scalar multi-load base preservation. Tests interpret produced instructions; they are not a hardware GPU execution test.

## Checked physical DWORD reads and grouped scalar address preservation

Reference pinned KytyPS5307fdcf7ad5d9d15b5966288fcc598ac9dbca188: backend/spirv/spirvEmitterMemory.cpp::LoadBdaDword (pointer conversion only inside present branch, explicit aligned DWORD load, RecordBdaFault for missing backing) and frontend/translate/Memory.cpp::S_LOAD (collect all loaded values before writing scalar destinations). Full prefix src/graphics/shader/recompiler/. No upstream implementation copied.

DefineGuestAddressRead emits physical-storage pointer conversion and a 4-byte-aligned OpLoad only after successful range lookup and guest/device alignment checks. Returns uvec2 {DWORD,status}, where 0=success,1=missing/boundary,2=unaligned. A real zero payload with status0 is distinct from a failed zero payload. Native module enables Int64 and SPV_KHR_physical_storage_buffer/PhysicalStorageBuffer64 only when the explicit address-table flag is set. Dynamic execution remains disabled; callers must consume/report status rather than silently substituting zero.

SrtPatcher preserves ScalarMemoryGroup metadata for groups containing dynamic components before partial substitutions erase their memory/source operands. Metadata is validated transactionally and includes pre-group SGPR pair and dynamic offset; fully static groups receive no capture overhead. Enabled-table emission captures all three source values before the first component, even when that component has become LoadSrtDword. Register collection includes these hidden sources. The saved SSA values are prepared for subsequent scalar dynamic-load integration.

Extended production SPIR-V interpreter tests compose actual lookup and read functions over discontiguous backing, high device addresses, real zero payload, missing and unaligned pointers. Rejected reads must issue zero physical memory accesses. The called physical-read module passes Vulkan1.1 spirv-val. Additional actual SrtPatcher tests cover partial substitution, overlapping source/destination writes, fully static groups and malformed-group rollback. Test success CI38093889853; full build/artifact verification pending final status below. These are instruction interpretation/validation tests, not hardware GPU execution.

Final validation: source505ae7240cbc7cfcd4a6f3860975367f643aa98f, CI38093889853 all regressions and full Windows build succeeded. Verified archive digest and all19 manifest entries under D:/proje/psemu-codex/work/ci-astro-physical-read, archive outputs/psemu-windows-x64-astro-physical-read.zip. Installed Sarah loader preserved. No Astro runtime test because checked reads are not yet connected to the failing dynamic SLoad and dispatch fault/coherence preparation is unfinished. No first-frame claim. Next: consume read status in dynamic scalar emission, report missing guest addresses after fence and prepare their backing coherently; then enable a narrowly supported path for an actual Astro test.

## Atomic first-fault reporting for checked address reads

Reference remains KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188, src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp::RecordBdaFault and LoadBdaDword. Upstream bitmap indexing assumes its address layout. This original adaptation instead records one exact 48-bit guest address and read status in a four-DWORD SSBO, avoiding a dense Windows address bitmap or an unbounded append queue.

DefineGuestAddressFault claims status with Device-scope OpAtomicCompareExchange; only the winning invocation writes address halves. A zero success status does not claim the record. The record must be cleared before dispatch and read only AFTER its fence: status is claimed before address publication and is not an in-flight ready flag. Later faults cannot overwrite the first record. This deliberately diagnoses the first failure rather than collecting all missing pages.

DefineGuestAddressRead optionally calls this reporter on every rejected path before returning its explicit status. Existing callers default to no reporter; dynamic runtime execution remains disabled. The test module binds a separate fault SSBO and calls the production reporter from the production reader. Interpreter tests verify original high guest address retention, success/real-zero silence, first-writer retention and missing/unaligned status reporting. The operation interpreter is sequential; actual concurrent GPU execution is not tested here. Vulkan SPIR-V validation and full Windows CI results will be recorded below.

Host integration audit: CommandBuffer::WaitForFenceOnly establishes completion, while FinalizeFence resets the host stream before ReleaseResourcesAfterFence. A future diagnostic readback must therefore use retained dedicated backing, or consume it before stream reset; do not retain only a stream offset. BufferCache::ObtainBuffer's small CPU-dirty read fast path returns a streamed allocation, so preparing the resident BDA owner requires bypassing that path explicitly. No fault dispatch binding, host readback/population or scalar dynamic fallback is enabled by this change.

Validation: corrected source1bba310f803dd7db33f4221952e8117874c73b54, CI38094849421 all regressions, called fault/read SPIR-V Vulkan1.1 validation and complete Windows build succeeded. Initial run38094626823 detected a SelectionMerge placement error; fixed before successful validation. Archive SHA256 and all19 manifest entries verified under D:/proje/psemu-codex/work/ci-astro-address-fault, outputs/psemu-windows-x64-astro-address-fault.zip. No runtime test: dynamic scalar execution remains disabled and the previous Astro PC0x2030 blocker is unchanged. Next: dedicated fault descriptor/readback after fence and coherent resident backing preparation, then dynamic scalar emission/status handling. No first frame.

## Fence readback, resident coherence, scalar fallback and BVH adaptation

Compared KytyPS5 pinned307fdcf7ad5d9d15b5966288fcc598ac9dbca188: src/graphics/host_gpu/renderer/cache/faultManager.cpp::ProcessFaultBuffer, RenderContext::PrepareBda, cache/bufferCache.cpp::SynchronizeBuffer, and src/graphics/shader/recompiler/frontend/translate/Memory.cpp::S_LOAD. psemu now binds a dedicated host-coherent 16-byte GuestAddressFault SSBO, clears it before recording and reads it after WaitForFenceOnly completes. CommandBuffer::End records shader-write to host-read visibility; the pre-dispatch host-write barrier exposes initialization. Completion actions retain their backing, run once after submitted work completes and are discarded without reading unsubmitted recordings. Completion precedes host stream reset and descriptor/resource release. Nonzero read status exits with its exact address: partially executed outputs are never claimed correct, and no replay/page population is silently assumed.

ObtainBuffer's force_resident parameter bypasses the small CPU-dirty stream fast path. PrepareAddressSnapshot prepares cached owners, rejects image-owned GPU bytes/metadata, checks recording generation and recaptures a current bounded snapshot. GPU-dirty buffer owners themselves remain authoritative, with write-to-read visibility before physical shader access. Missing owners still fail address lookup; memory preparation covers existing resident cache entries, not every guest allocation.

Explicit CompileOptions::allow_guest_scalar_reads (default false, compute-only) and startup PSEMU_GPU_SCALAR_READS=1 select the restricted raw DWORD scalar path. ResourceTracking preserves unknown runtime SGPR addresses without CPU descriptor materialization, but rejects typed/formatted/wrong-width contracts transactionally. SrtPatcher captures x1..x16 source pairs/offsets before overlapping writes. EmitGuestScalarAddress masks offset alignment like the existing scalar path, independently widens dynamic/component offsets and retains noncanonical invalid bases rather than wrapping into a valid allocation. Checked status preserves the old destination on failure while recording the error for fence-time diagnosis. Other memory opcodes keep their existing resource contracts.

Adapted actual software intersection arithmetic from upstream backend/spirv/spirvEmitterBvh.cpp into guestBvhIntersect.h; attribution and upstream GPL-2.0 LICENSE retained in docs/kytyps5-bvh-license.txt. IR integration snapshots descriptor/ray operands before overlapping result writes and calls the helper only under active EXEC. Triangle kinds0..3, half boxes4/full boxes5, descriptor/node/ray validation, barycentrics, sorting and growth follow upstream. Each full box block is independently resolved for all64 bytes through the sparse lookup; missing or misaligned physical backing reports the original guest block before pointer dereference. Invalid descriptors/rays retain the upstream architectural miss result.

Tests/guest_bvh_semantic_test.h interprets production emitted intersection instructions for triangle hit/miss, invalid descriptor, NaN ray, full/half boxes, sorting, separated physical blocks and absent first/second blocks. The lookup test also emits/calls actual scalar/read/fault/BVH functions for spirv-val. These are interpreted instruction tests and module validation, not hardware execution. Additional table tests cover exact-once completion, retained resource lifetime and discard of unsubmitted work. Runtime opt-in validation and full Windows build results pending below.


Integrated validation: source31a9be6c5944d09b66efa006b5dca7ba069f979c, CI38095971042 passed all regressions, Vulkan1.1 SPIR-V validation and complete Windows build. Archive digest and all19 manifest entries verified in D:/proje/psemu-codex/work/ci-astro-bvh-integrated. Actual Astro launch with PSEMU_GPU_SCALAR_READS=1 passed the previous compiler blocker atPC0x2030 but failed resource tracking atPC0x2150 (s_buffer_load_dword with a runtime descriptor). This is shader compiler progress only: no intersection dispatch or first frame confirmed.

## Runtime scalar buffer descriptors and grouped source capture

Compared pinned KytyPS5 backend/spirv/spirvEmitterMemory.cpp::LoadIndirectScalarBuffer. The restricted read-only scalar buffer path preserves the four descriptor DWORDs and dynamic offset before the first component, including a component replaced by LoadSrtDword. x1/x2/x4/x8/x16 retain pre-group sources when loaded destinations overlap the descriptor. Typed/formatted/cache-modified or other unknown buffer operations retain existing rejection. The shader helper uses aligned base/offset and widened max(stride,1)*records bounds; architectural out-of-bounds returns zero with success, whereas absent physical backing keeps checked-read failure/fence diagnostics. Original psemu adaptation, no additional upstream code copied.

Source80fa14de8084e864bf02f5e5d475c9331bf161b0 tests all four wide counts with both fully runtime groups and a first-component SRT substitution, source/destination overlap, descriptor/offset capture, stride and offset carry, architectural OOB and missing backing. CI38097006101 and actual runtime validation pending. Existing resident-cache coverage still does not populate every missing guest owner; no automatic replay or first-frame claim.

Validation update: CI38097006101 succeeded for source80fa14d, including all regression tests, called Vulkan1.1 SPIR-V module and complete Windows build. Verified package archive digest and19 manifest entries under D:/proje/psemu-codex/work/ci-astro-scalar-buffer-wide. Actual runPID4896 compiled17 shaders but then failed with host access violation0xc0000005, read0x24 atloader RVA0x2a38f8. No explicit PC0x2150 resource failure appeared, but this does not establish that the BVH shader compiled or dispatched. Repeat runPID10940 captures host base0x7ff7bf3d0000. Source9ae91b7 adds a separate CI symbol-map artifact for exact native crash diagnosis; runtime behavior unchanged. First frame still unconfirmed.

Native crash diagnosis: repeatPID10940 also failed inside SrtWalker Evaluator atloader RVA0x2a38f8; disassembly showed a direct DWORD host load from computed guest address. Added Windows ReadProcessMemory fallback for SRT reads without a supplied reader, staging the value and rejecting unreadable addresses without publishing partial output. This is failure diagnosis, not fabricated data or execution bypass. Compared upstream ir/passes/SrtWalker.cpp::EvaluateRawRead reader/failure contract; upstream also directly reads scalar memory when no callback is supplied, so this Windows guard is a local adaptation. Regression covers unreadable0x24, diagnostic address, unchanged flat output and a real successful stack DWORD.

Sourceaa39bcaafd5636ec06a4c5e93f63be6f35894674, CI38097747739 passed all regressions, Vulkan1.1 module validation and full Windows build. Runtime package SHA256 and19 manifest files verified under work/ci-astro-srt-read-guard; separate symbol archive digest verified in work/symbols-astro-srt-guard. ActualPID18296 terminated with a structured shader error rather than host0xc0000005: computePC0x1f6c ReadConst failed at0x18. Captured raw14384-byte shader; prior IR identifies s_load_dword vcc_lo,s8,+24, in a block reached after EXEC-dependent branching. SRT planning currently eagerly flattens standalone immediate-offset reads before control-flow execution; upstream evaluates runtime uniformity/active masks. Next: keep non-descriptor scalar reads in the GPU execution path under the explicit compute opt-in, preserving required CPU descriptor dependencies and control-flow semantics. First frame and BVH dispatch remain unconfirmed.


## Keep standalone scalar reads under original GPU control flow

Reference remains pinned307fdcf7ad5d9d15b5966288fcc598ac9dbca188, ir/passes/SrtWalker.cpp runtime active-mask/uniformity validation. Local adaptation moves the explicit compute guest-scalar-read flag before SRT planning. PlanBuilder still collects required CPU descriptor dependencies, then omits eager standalone scalar-read collection when that flag is enabled. Remaining unpatched reads use the existing checked GPU path and pre-group source capture within original shader blocks. There is no PC-specific bypass, replacement payload or ignored fault. Default-disabled programs retain their existing flat SRT plan.

Regression in tests/scalar_address_shift_test.cpp: same immediate readPC0x1f6c is flattened with default options, retained for GPU execution under opt-in, and still collected when an actual CPU descriptor depends on it. First CI38129231440 failed because the fixture incorrectly used reserved provenance IDs0/1 as constants; corrected in sourcef35b62238a8a21f35c64409d23286c8154cf2ff6. CI38129457414 and Astro runtime outcome pending.

Validation: corrected sourcef35b62238a8a21f35c64409d23286c8154cf2ff6, CI38129457414 passed all regressions, SPIR-V validation and complete Windows build. Verified archive SHA256 and19 manifest entries at D:/proje/psemu-codex/work/ci-astro-scalar-control-flow, archive outputs/psemu-astro-scalar-control-flow.zip. ActualrunPID9772 compiled18 shaders, including the previous14384-byte compute shader now dumped as 0004_0017_new_shader_cs_00000185aa5d1000.bin (successful prefix). The PC0x1f6c host SRT failure no longer appears. This establishes compilation only, not successful BVH dispatch or first frame.

New actual blocker is NativeTexture storage binding: resource/descriptor/format valid, encoding rejected. Captured descriptor dwords85be0e00,c4700001,010dc1df,91b00fac,00000000,00700000,807b0000,01861791; format71/tile27, write-only storage2D1920x1080, metadata control0x7b0000. IsSupportedStorageTextureEncoding currently requires words6/7 zero. Pinned upstream host_gpu/renderer/pipeline/descriptors.cpp (DCC discovery near627), renderer/cache/textureCache.cpp, image/imageInfo and colorRenderTarget.cpp propagate DCC range/alpha metadata and maintain its ownership. Local renderer lacks general DCC color texture metadata; existing HTile and video-out compression handling do not provide it. Next work must integrate metadata sizing, state/coherence and expanded native image contents before allowing this encoding. Do not just accept words6/7 or ignore the metadata. Runtime exited explicitly before first-frame confirmation; Sarah installed loader and user scripts preserved.


## DCC metadata footprint and uniform-slice interpretation foundation

Compared pinned307fdcf7ad5d9d15b5966288fcc598ac9dbca188 guest_gpu/tile.cpp::TileGetDccSize/TileGetColorMetadataSize and host_gpu/renderer/cache/textureCache.cpp::DecodeColorClear/MaterializeColorClear. New dccMetadata.h is an original small adaptation: one-mip single-sample 4KiB metadata raster, power-of-two element sizes1..16, checked32-bit allocation size, aligned48-bit guest range, and transactional layout output. Astro1920x1080 RGBA16F (8 bytes/texel) covers512x256 texels per metadata block,4x5 blocks/81920 bytes.

InspectDccSlice requires exact expected slice length and identical bytes throughout. Uniform0xff means expanded;0x00/0x40/0x80/0xc0 produce architectural clear colors with alpha-position handling. Register clear0x20, mixed/compressed/unknown codes and partial slices remain unsupported. No GPU or CPU backing is read by this helper. It does not establish synchronization, update guest metadata or initialize an image; those are required at the call site before compressed descriptors can be accepted. The helper is not connected to NativeTexture/TextureCache yet, so the known storage-encoding blocker remains unchanged.

Regression tests/dcc_metadata_test.cpp covers captured extent/metadata address, footprint boundaries, three-slice layouts, overflow with unchanged output, invalid formats/mips/addresses, all supported clear codes, alpha order, expanded bytes, mixed last byte and register clears. Sourcee289237375cc9f8932c32a660718ca465c70427f, CI38130392021 DCC tests passed; full Windows/regression validation pending.

Validation completed: CI38130392021 succeeded across all regressions, DCC tests, SPIR-V validation and complete Windows build for sourcee289237. No runtime launch: renderer does not yet call the new helper, so an identical Astro run would still reject the same compressed storage descriptor. No package installed or first-frame claim.

Integration audit: local BufferCache::ObtainBufferForImage performs ordered GPU readback and can publish CPU-current metadata backing, but may submit/reset recording; call it outside texture locks and retry recording preparation when generation changes. TextureCache::FindStorageTexture currently detiles guest color unconditionally; DCC clear slices must instead initialize their native subresource with the architectural clear, while expanded slices may use normal detiling. Metadata changes, alias retirement, unmapping and image-to-buffer readback must carry associated metadata ownership; existing HTile RegisterMeta and ClearMeta are not a complete color-DCC implementation. Required next work is metadata association/lifetime and coherent slice snapshots before enabling accepted descriptor bits. Unknown compressed or register-clear slices stay explicit failures.


## Coherent DCC snapshot integration and actual metadata measurement

Compared pinned307fdcf7ad5d9d15b5966288fcc598ac9dbca188 host_gpu/renderer/cache/textureCache.cpp::MaterializeColorClear: publish GPU-dirty metadata backing before inspecting complete slices, never hold the texture lock across submission. TextureCache::CaptureDccMetadata uses local BufferCache::ObtainBufferForImage under ResourceMutex, rejects image-owned/GPU-virtual metadata overlap and checks command recording generation before publishing an immutable result. ReadDccSlices stages outputs, refuses invalid ranges/failed readers, and inspects each complete slice. Tests additionally cover one read, invalid-address no-read and unchanged results on reader failure.

NativeTexture probes only the captured RGBA16F/tile27/one-layer/one-mip metadata control0x7b0000 before existing storage validation. If coherence preparation resets recording it returns a provisional empty binding that the enclosing generation check discards before descriptor use. Bounded diagnostics record slice kind and architectural clear color. This is an inspection path, not permission to ignore compression metadata; encoding validation remains unchanged and rejects binding.

Sourceb6fec136025d0c2e693a2c318ecbe3b7adb5b8ce, CI38131069463 all regressions/DCC/SPIR-V/full Windows build succeeded. Artifact archive digest and19 manifest entries verified in D:/proje/psemu-codex/work/ci-astro-dcc-snapshot (outputs/psemu-astro-dcc-snapshot.zip). ActualPID19488 read metadata0x1f2191e8000,81920 bytes, one complete slice; kindClear, color0,0,0,0. Runtime then reached the unchanged explicit unsupported storage encoding. No native clear recorded, DCC cache association/lifetime still incomplete, no first frame or successful BVH dispatch confirmed. Diagnostic newline formatting corrected after measurement.

Next integration: actual Vulkan clear of the native subresource and expanded metadata publication, with associated native image ownership, metadata invalidation/unmapping/alias retirement and existing image readback. Do not replace compressed color with unverified zeros: measured uniform DCC clear is an architectural clear operation, and unknown/mixed/register-clear codes must continue to reject. Existing PrepareHostWrite rejects GPU-owned/storage aliases, so bulk CPU color rewriting would bypass current ownership contracts; use the native clear path instead.


### Native DCC clear initialization (staged, 2026-10-11)

Added an optional initial clear to FindStorageTexture for newly-created single-level 2D RGBA16F images. It records transfer-destination barrier, Vulkan clearColorImage, and general-layout barrier instead of detiling stale compressed color backing; the image becomes GPU-modified and is retained through the fence. Existing owners reject this initialization. An ordered buffer readback that resets recording returns before image mutation.

Reference: KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188, src/graphics/host_gpu/renderer/cache/textureCache.cpp::MaterializeColorClear. This native operation is staged: descriptor callers still use the original upload path and reject DCC encoding. Metadata association, expanded-key publication and mutation/unmap lifecycle remain required before enabling it. No runtime advancement or first frame is claimed. DCC regression covers clear-kind/color/format/dimension/mip restrictions.

Validation: source 78fece6bfe4bd6e65df5adb1fcc715912a56760d; GitHub Actions run 38145521963 completed successfully, including all regressions, DCC tests, guest-address SPIR-V validation and the complete Windows emulator build. The subsequent source edit changes indentation only. No installed loader was replaced and no game run was used to claim DCC runtime progress.

2026-10-11 additional reference: inspected Jetsku/KytyPS5 at f9e19582f0deac0d1226a7ce3faae30a9ab408ab. See docs/jetsku-kytyps5-reference.md. Prioritize successful-clear-only key consumption, metadata content revisions and eliminate-before-sampling regression before GPU-side DCC inspection or performance optimizations. Current runtime blocker is unchanged.


2026-10-11 DCC storage integration: captured one-level 2D RGBA16F descriptor (tile27, swizzle0xfac, control0x7b0000) now reaches a transactional full metadata read, native clear and expanded-key publication. The color image remains GPU-authoritative and fence-retained. Rebind reads the entire key again; expanded keys preserve subsequent draws and a rewritten uniform clear is applied anew. Associated metadata cannot alias another image, virtual metadata or a different color owner. Full metadata unmap detaches the consumed association without retiring native color; partial unmap remains explicitly unsupported. Unknown/mixed/register clears, sampled DCC, other encodings and general decompression remain unsupported. References: Jetsku f9e19582, textureCache.cpp::MaterializeDccClear (only successful clear consumes keys), dccClear.h lifetime contract, ShaderRecompilerComputeTests.cpp clear/rebind regressions. No GPU-side metadata inspection optimization or source wholesale import. Validation/runtime result pending.

Validated DCC integration: source2aefdb3, CI38147300368 all regressions/SPIR-V/full Windows successful; artifact digest and19 manifest files verified. Actual Astro PID20212: DCC-CLEAR color0x1543b2e0000 metadata0x15494b18000 bytes81920 native=1 consumed=1, then later storage binding advanced. No first frame or completed BVH dispatch claimed. Next fatal: BufferCache::ObtainBuffer during PrepareAddressSnapshot, readonly32KiB at0x1547d9b8000. Matching verified loader.map confirms caller; logs tools/data/astro_dcc_native*. The buffer GPU-dirty/image-not-GPU-owned case was rejected by the older coherent-read guard. Added narrow reuse when ALL byte-overlapping images explicitly have buffer_modified ownership; competing or mixed owners still reject. Reference: Jetsku bufferCache.cpp::ObtainBufferNow and GPU write image invalidation preserve native buffer ownership; no shader/address fault is suppressed. New guard validation/runtime pending.

Second runtime checkpoint: b39e29d, CI38148594482 all tests/SPIR-V/full build passed; artifact hash and19 files verified. PID20388 (base0x7ff75f160000), logs astro_dcc_buffer_ownership*: DCC clear consumed81920 bytes again, but the same32KiB snapshot alias still rejected with buffer_gpu=1,image_gpu=0,buffer_owned_images=0. Thus the narrow already-yielded guard did NOT fix the captured instance. Added explicit existing InvalidateMemoryFromGPU transition for a readonly GPU-native buffer with no competing GPU image. The classifier must validate the image kind, containment, alignment and sole owner, then the region is rechecked; otherwise it still fails. This repairs a clean sampled view published after buffer write ownership was established, preserving resident GPU bytes and marking the view stale for subsequent refresh. Pending final build/runtime; no first frame claim.

Third runtime: ca8dec9, CI38149657319 all checks/full build passed; artifact digest and19 entries verified. PID17700 base0x7ff63e810000, astro_buffer_alias_refresh*: DCC passed again, but raw alias invalidation correctly rejected buffer0x11e79268000+0x8000 overlapping sampled image0x11e78a69000+0x800000. The buffer page overlaps the last4KiB of the image, extending28KiB past its end; page-alignment/containment assumptions do not hold. Added exact GPU write-range proof before image invalidation: only clean sampled images whose bytes intersect NO dirty write may coexist with a GPU-native readonly buffer. Exact dirty intervals come from BufferCache RangeSet under its lock; image proof uses the separate image lock under ResourceMutex. Empty/invalid intervals, any dirty byte in any image, GPU-owned images or other image kinds reject. Captured edge-page regression includes adjacent, crossing, mixed and overflow intervals. Reference: Jetsku bufferCache.cpp::TryFalseSharingWrite and exact ownership tracking in ObtainBufferNow; adapting the byte-vs-tracker-page distinction, not bypassing dirty ownership. Pending new validation/runtime; no first frame.

Fourth runtime: 153e7d7, CI38150552819 all checks including edge-page regression/full build passed; digest and19 files verified. PID15848 base0x7ff6a2910000, astro_buffer_edge_read*: DCC passed; BUFFER-PADDING-READ addr0x2a485d98000 size0x8000 exact_writes1 images_unchanged1 proved the captured edge alias was resolved. Next fatal: address table could not prepare coherent resident backing. Added snapshot restriction to buffer fragments whose pixels are not currently native-image/virtual-metadata owned; original device offsets and shared backing lifetime are retained. Stale image bytes become absent lookup ranges, not exposed stale backing and not fabricated zero success. Any actual shader read into such a gap still records the existing GPU fault. Tests cover split offsets, crossing/gap failures, overlapping cuts, complete exclusion, malformed/overflow cuts and transactional output. Reference: Kyty/Jetsku cache ownership distinction behind BDA page availability; no general image-to-BDA readback implemented. Pending final validation/runtime; BVH dispatch and first frame remain unconfirmed.


Validated runtime checkpoint (2026-10-11): source 6236a10ce619322233b9aa4fe3a93a9fb6ba782d, GitHub Actions 38151625929 succeeded including all regressions and full Windows build. Artifact digest plus 19 manifest files verified; separate D: build ci-astro-coherent-address-fragments launched as PID17916. Logs: tools/data/astro_coherent_address_fragments_log.txt and stderr.txt. Previous coherent resident address snapshot fatal was passed; runtime compiled 26 shaders and reached additional RectList/triangle draw commands. New explicit fatal at descriptors.cpp:639: unsupported storage texture, resource=1 descriptor=0 encoding=1 format=1 kind=10 dimension=1 type=8 format=77 tile=27 swizzle=0xfac extent=1x1x1 size=0x10000, write-only. Captured words 6e829200,04d00001,00000000,81b00fac,00000000,00700000,00000000,00000000. Local storage validation/cache only admits 2D/2D-array/3D; next work is complete 1D storage image type, layout/view and ownership support, compared with pinned Jetsku pipeline/descriptors.cpp, cache/textureCache.cpp and shader/shaderBindings.cpp. No first frame or successful BVH dispatch claim; shader count and recorded draw commands alone do not prove GPU completion. Sarah loader and user launchers preserved. Astro verified launcher updated to this separate build.


Validated ES runtime (2026-10-11): source ebcf635d971fee9822677e2d361358efaa5cf3fc, CI38153988216 succeeded (all regressions/full build); artifact digest plus19 files verified. Separate ci-astro-es-resource launched PID14920, logs tools/data/astro_es_resource_log.txt and stderr.txt. ES-RSRC1 offset0xca value0x03000002 preserved=1 confirms the prior indirect SH fatal passed. Next fatal TextureCache.cpp:2845 unsupported GPU invalidation alias: formatted buffer request0x19d6ee60000+0x480000 intersects GPU-authoritative RenderTarget(kind2)0x19d6e170000+0xff0000, buffer_modified0, ambiguous0. Image ends0x19d6f160000, request ends0x19d6f2e0000: partial overlap extends0x180000 beyond image. Need a supported ordered render-target-to-buffer ownership transfer preserving untouched bytes before formatted writes; must not simply invalidate/discard GPU pixels or remove the classifier guard. Existing ClassifyBufferImageWrite and render-target synchronization paths are the next audit targets, compared with pinned Jetsku cache ownership/fill/write transitions. Still no confirmed first frame or successful BVH GPU dispatch. Verified Astro launcher points to ebcf635 separate build; installed Sarah loader/user launchers untouched.


Validated rebind runtime (2026-10-11): source1efbe419ff22f0eea5b0e1f9eb8416fee18b089e, CI38155943069 passed all regressions/full build; artifact digest plus19 files verified. Separate ci-astro-rt-buffer-rebind launched PID20716, logs tools/data/astro_rt_buffer_rebind_log.txt and stderr.txt. RT-BUFFER-SYNC image0x1f1e1060000+0xff0000 write0x1f1e1d50000+0x480000 again confirmed full publication. Prior render-target rebind fatal passed; compiled31-34 shaders. New explicit fatal in ReadbackWorker::DownloadColorImage at textureCache.cpp529: color-image readback storage unsupported addr0x1f1e1d50000 size0x480000 meta0 buffer1 kind2. A later sampled descriptor at this address is1920x1080 format14 (TEX#33); preceding descriptor retries indicate recording reset during resource preparation. Current CPU-fault readback rejects ANY cached buffer overlap, even after target rebind; future fix must validate exclusive current GPU image ownership, refresh overlapping native buffer fragments after full readback, and preserve fault/readback worker locking/page tracker state. Existing PublishReconstructedImageBacking and buffer CPU/GPU-owner guards can inform this seam; do not merely remove buffer_overlap check. Both buffer handoff and rebind now runtime-validated. No first frame or BVH GPU-dispatch completion claim. Verified Astro launcher points to1efbe41; Sarah remains unchanged.


Pending Astro render-target read-fault overlap fix (2026-10-11): DownloadColorImage admits cached-buffer overlap only for render-target CPU reads, with metadata/layout guards preserved. ReconstructImageBacking holds the buffer cache lock across competing-owner validation, GPU download/retiling, WriteBacking and publication. Competing CPU-dirty cached fragments or GPU-dirty buffer ranges reject before backing replacement. The active buffer read-fault page remains pending until Buffer Complete/Release (GpuResourceManager dispatches buffer before texture); other image pages are marked CPU-dirty immediately, and the final page is published after texture worker Completed, before submissions resume. Storage-image overlap and write-fault overlap remain unsupported. Production-extracted regressions cover transactional callback ordering, multi-fragment publication, exterior GPU bytes, CPU/GPU conflicts and deferred fault-page publication. Reference: original307fdcf7 cache/textureCache.cpp SynchronizeBufferFromImage safe-download bounds; Jetsku f9e19582 cache/textureCache.cpp PreserveImagesForGpuWrite safe authoritative-image and competing-alias guards. Its historical image-start-inside-buffer restriction does not cover our previous partial image overlap. This is an adaptation, no imported source. Full CI/runtime validation pending; first frame remains unconfirmed.


Validated render-target read-fault runtime (2026-10-11): source360540707f55da241ea2452c35ea0c71ac5c2895, CI38157524647 all regressions/full Windows build success; artifact digest and19 manifest files verified. Separate ci-astro-rt-buffer-readback launched PID4748; logs tools/data/astro_rt_buffer_readback_log.txt and stderr.txt. RT-BUFFER-SYNC image0x1d90e600000+0xff0000 write0x1d90f2f0000+0x480000 followed by RT-BUFFER-READBACK image0x1d90f2f0000+0x480000 full_backing1 confirms previous readback-overlap guard passed. Subsequent graphics processing reached new fatal FindRenderTarget line1771: requested0x1d90e300000+0xff0000 intersects GPU-current StorageTexture(kind1)0x1d90e540000+0xc0000, same context, format22 extent480x270x1 pitch512 levels1 base_level0 tile24 type9 base_array0. Existing ClassifyStorageRenderTargetOverlap supports only exact address/size/format/shape native-image reuse; this smaller storage image is wholly contained in a different larger target. Need ordered storage-image publication/ownership transfer preserving current pixels and remaining guest/buffer backing before target reconstruction, supported download/layout and retirement isolation/fence lifetime proofs; do not simply retire GPU-current storage or relax exact-native copy guards. No first frame/present or BVH GPU completion verified. Astro launcher updated to3605407; installed Sarah loader and user launchers unchanged.


Pending contained storage-to-render-target reconstruction (2026-10-11): captured R32Float480x270 pitch512 tile24 type9 allocation0xc0000 lies wholly inside new0xff0000 render target. Narrow proof requires GPU-exclusive, CPU-clean storage, same context, supported single-mip depth-storage footprint, page alignment, complete containment, coherent target and NO cached buffer overlap across the entire target. Exact native-image reuse remains unchanged. Prescan under resource/texture locks detects only the new case; then GraphicsRunSynchronizeCommandProcessors calls existing SynchronizeGpu/FinishCommandProcessors outside cache locks to submit/wait all recorded producer writes. Ownership revalidated afterwards; all retirement isolation and pairwise GPU-owner checks precede storage download/retiling/WriteBacking via existing SynchronizeColorImageToBufferLocked. Only full old storage range is published, old Vulkan object retained through command fence, yielded marker cleared after successful reconstruction, normal retirement and full target upload follow. Reject native expansion mixed with reconstruction and competing storage owners. Regressions capture containment/dirty/context/buffer/layout rejection and480x270 bit-exact depth roundtrip. Reference original307fdcf7 cache/textureCache.cpp SafeToDownload/CanDownload; Jetsku f9e19582 cache/textureCache.cpp PreserveImagesForGpuWrite authoritative-image/alias guards; adapted without source import. CI/runtime pending; no first frame claim.

Storage reconstruction padding audit: the native Vulkan image excludes allocation padding. New TileConvertLinearToTiledDepthPreservePadding uses the same production depth permutation but does not clear the destination. Only the new contained-storage reconstruction opts in, preloading guest backing via TryReadBacking and replacing actual pixels; existing tiling APIs retain zero-padding behavior. Regression uses a full pixel mask, checks exactly width*height pixel stores, unchanged every padding word and external sentinels at480x270 and edge extents.


Validated storage reconstruction runtime (2026-10-11): sourcec0feaf9ae76178a5a408016c33f79ce487240b22 (includes de713a6 ordered drain/containment proof), CI38158823866 all regressions and full Windows build success. Artifact digest and19 files verified; separate ci-astro-storage-rt-reconstruct launched PID18464, logs tools/data/astro_storage_rt_reconstruct_log.txt and stderr.txt. RT-BUFFER-READBACK continues passing. STORAGE-RT-RECONSTRUCT storage0x1b0e5410000+0xc0000 target0x1b0e51d0000+0xff0000 full_backing1 confirms old storage alias fatal passed; reached35-37 shaders and another RT readback0x1b0e6640000+0x870000. Source object destruction follows completed native readback/fence. New fatal ResolveStorageImageOverlaps line1071: requested storage0x1b0e61c0000+0x1a20000, existing RenderTarget(kind2) same base+0x480000, same context, cached/tracker GPU0/0, buffer_modified0. Existing classifier permits sampled retirement but rejects render-target byte aliases even when the previous readback made backing CPU-current. Next audit: support retirement of clean CPU-current target for larger storage view with buffer coherence, tracker ownership, page-isolation and retained Vulkan lifetime proofs; preserve GPU-current/mixed-owner failures, do not widen by merely calling target sampled. No first frame/present or BVH dispatch completion verified. Astro launcher nowc0feaf9; installed Sarah binary/user launchers unchanged.


Pending clean render-target retirement for larger storage (2026-10-11): captured new storage same address0x1b0e61c0000+0x1a20000 contains old target+0x480000 after successful readback, cached/tracker GPU0/0 and buffer_modified0. New separate CanRetireCleanRenderTargetForStorage proof requires full containment, page alignment, same context, single-mip/layer target, no native GPU or yielded-buffer ownership, and coherent requested backing from the existing whole-range source. ResolveStorageImageOverlaps receives the command and actual source-coherence verdict; accepts only RenderTarget records passing this proof, preserves the existing sampled/storage/depth and GPU-current failures. Retirement isolation precedes mutations; old shared cache record retained until command fence and normal RetireImages tracker checks apply. The existing new-storage upload then reads full current guest backing. Regression captures dirty/mixed ownership, incoherent source, context, truncated/outside/unaligned/overflow ranges and array target rejection. References original307fdcf7 renderer/cache/textureCache.cpp current-owner selection and image retirement; Jetsku f9e19582 same path DeleteImage deferred lifetime/SafeToDownload ownership policy. Adaptation only; CI/runtime pending, no first frame claim.


Validated clean-target-to-storage runtime (2026-10-11): source24564a3cf22d995649b5bd4076047459668f2dd5, CI38160409638 all regressions/full Windows build success. Digest plus19 manifest files verified; separate ci-astro-clean-rt-storage launched PID18900, logs tools/data/astro_clean_rt_storage_log.txt and stderr.txt. RT-STORAGE-RETIRE confirms two CPU-current targets0x1bc6c390000+0x480000 and0x1bc6c810000+0x870000 safely retired for new storage0x1bc6c390000+0x1a20000. Previous storage-byte alias fatal passed; reached38-41 shaders and another supported storage reconstruction0x1bc6aea0000+0x280000. Next explicit fatal MarkGpuWritten line2644: GPU-written image aliases dirty buffer at0x1bc6c390000 size0x1a20000, buffer CPUmodified1 GPUmodified0. Same large storage range later becomes render target. Current ObtainBufferForImage refreshes a containing cached native buffer, but returns CPU-current guest source for non-containing/multi-fragment ranges without necessarily consuming all cached-fragment CPU dirtiness. Next audit should establish all overlapping cached-fragment coherence before image GPU writes, distinguish untracked/default-CPU-dirty holes from actual native owners, preserve external bytes/competing GPU owners and fault/recording order; do not just remove MarkGpuWritten dirty-buffer guard or mark stale native buffers clean. Runtime confirms retirement progress, not first frame or BVH completion. Astro launcher now24564a3; installed Sarah loader and user launchers unchanged.


Pending native buffer fragment coherence fix (2026-10-11): ObtainBufferForImage now calls RefreshCpuImageFragmentsLocked after ordered GPU-buffer readback, before containing-owner lookup. Validates every clipped native fragment/GPU conflict before any upload, refreshes only CPU-dirty cached intersections from physical guest backing, preserves exterior bytes and cache-hole tracker defaults. Containing-source duplicate upload removed; source.cpu_dirty retains initial verdict for image refresh. New IsCachedRegionCpuModified checks actual resident intersections; only MarkGpuWritten image-vs-buffer guard uses it so unowned CPU-dirty tracker holes are not mistaken for native GPU owners. Genuine dirty native fragments and GPU conflicts remain rejected. Production-extracted tests simulate two partial owners with a hole, verify every byte against guest/exterior values, no repeated clean upload, untouched exterior GPU bytes, and rejection before any writes for tracker GPU/range GPU/invalid later owner. References original307fdcf7 renderer/cache/bufferCache.cpp SynchronizeBuffer/ForEachUploadRange; Jetsku f9e19582 same file ObtainBufferForImage containing-vs-staging source distinction. Adapted to local fragment cache/ownership with no imported source. CI/runtime pending; no first-frame claim.


Validated native buffer-fragment coherence (2026-10-11): source a231c7c48f645b21e756ecb545a8bd2dd6ee2fa6, CI38169241461 all regressions/full Windows build success. Archive digest plus19 manifest files verified; separate ci-astro-buffer-fragments launched PID1548. Logs tools/data/astro_buffer_fragments_log.txt and stderr.txt. IMAGE-BUFFER-FRAGMENTS image0x19963f80000+0x1a20000 confirms5 uploads totaling0xcf0000 before RT-STORAGE-RETIRE; previous CPU-dirty buffer fatal passed. Reached42 compiled shaders. New explicit fatal MarkGpuWritten textureCache.cpp2644: image0x1992f9a0000+0x1fe0000 CPUmodified0 GPUmodified1; stderr VO-REFRESH at same address reports KEEP-GPU. Existing RefreshVideoOutBacking early return includes unconditional kind==VideoOut, bypassing subsequent buffer-current refresh/ownership checks. Next audit must establish actual VideoOut compression/layout and native buffer producer ordering, then transfer current buffer contents before image writes; retain GPU-conflict and unsupported compressed-source guards rather than bypassing them. Original307fdcf7 cache/textureCache.cpp UpdateImage(~981)/ObtainBufferForImage(~993), Jetsku f9e19582 same path VideoOut ownership(~2093-2170), buffer source(~2341,2532) identified for next comparison. No first frame/present or BVH completion verified. Verified Astro launcher points toa231c7c; installed Sarah loader and tracked user launchers unchanged.


Pending VideoOut owner refresh (2026-10-11): captured uncompressed3840x2160 tile27 format58 surface0x1992f9a0000+0x1fe0000, no metadata/DCC. Removed unconditional VideoOut-kind early return; keep native GPU image only with no CPU/buffer competing owner. Resident-buffer CPU query avoids default tracker holes. Existing ordered ObtainBufferForImage readback then coherent guest upload refreshes uncompressed surfaces; buffer_modified clears after upload. Compressed dirty-source and mixed native-owner rejection retained. Production-extracted refresh tests cover clean native preservation, CPU upload, GPU-buffer handoff (including clean image tracker), repeated no-op, incoherent source and compressed/mixed-owner rejection. Original307fdcf7 cache/textureCache.cpp UpdateImage ~975-1005, Jetsku f9e19582 BuildTextureTransfer ~2090-2155 and UploadImage ~2178 inspected: buffer-current uncompressed surfaces require upload; native owner alone may stay resident. Adaptation only. CI/runtime pending; no first frame claim.


Validated VideoOut buffer refresh (2026-10-11): source8b57accb67b2282264e6650277999e1f40cd374d, CI38170477876 all regressions including production VideoOut refresh test and full Windows build success. Package digest plus19 manifest files verified; separate ci-astro-video-out-refresh launched PID1548. Logs tools/data/astro_video_out_refresh_log.txt and stderr.txt. VO-BUFFER-REFRESH addr0x199daca0000+0x1fe0000 buffer_source1 confirms uncompressed buffer ownership transfer/upload; previous MarkGpuWritten GPU-dirty buffer fatal passed. Reached44 compiled shaders. New explicit fatal GetStorageTextureSampledView imageView.cpp382: type8 depth1 base0 levels1 view_levels1 image_levels1 base_array0. Existing single-row type8 storage image normalization is supported for writes, but sampled view shape selection needs audit alongside sampled descriptor dimensionality/emitter; preserve native image/view compatibility and array/mip/swizzle/format checks. First frame/present remains unverified. Astro launcher points to8b57acc; installed Sarah and tracked user launchers unchanged.
