# Astro local CRT and thread startup investigation (2026-10-14)

The game still has no verified visible framebuffer. All tests and tools run from D:.
The installed Dreaming Sarah loader and the user's launcher changes are preserved.

## Observed failures

- `astro_local_resource_args_log.txt`: at the real Animation component callback
  (RVA 0x37f250), the linked resource node carries a null payload. The subsequent
  read at RVA 0x37f34c fails. ISound has a non-null payload; ModelRendering is null.
- Secure CRT imports `strcpy_s`, `sprintf_s`, `memcpy_s`, and `memmove_s` were
  falling through to a success stub without writing the requested data. Implemented
  bounded operations in the existing SysV HLE route, with guest pointer checks.
  Early runtime calls now copy actual render resource names and format integers.
- The first full secure-copy run failed in audio thread entry at RVA 0xe97510.
  Astro's wrapper checks its own pthread handle before loading its argument object.
  The old `CreateThread` could enter before the HLE published that handle.
- `CreatePublishedGuestThread` creates suspended, publishes the output, then resumes.
  512 immediate-entry tests checked that the guest-visible handle identifies the
  currently executing thread. The real game run `astro_local_thread_start_log.txt`
  passed audio thread creation at T+67.71 and loaded credits XML at T+73.13.
  It later failed at the prior Animation RVA 0x37f34c; exit code 0xc0000005.
- In that run, `strncat_s` was also a success stub. Three calls at caller return RVAs
  0x4fc682 / 0x4fc6bc / 0x4fc6e7 preceded opens of only `data/prein/effects/`,
  followed by the game's missing skeleton/graphics assertions. Implemented bounded
  append and added a test assembling an actual skeleton path from directory,
  basename, and extension. Runtime verification is in progress.

## Validation

- `test_guest_bounded_string.ps1`: path copy, formatting and append; truncation,
  overflow, unterminated destination, and buffer guards.
- `test_guest_safe_memory_copy.ps1`: binary zero bytes, overlap, range/null errors,
  destination preservation for memmove errors, guard preservation.
- `test_guest_thread_start.ps1`: 512 immediate-entry handle checks and invalid args.
- All three pass; local Windows loader builds successfully.
- Audio context/event flag output stubs remain separate work; no claim that the
  complete audio engine works, and no null-read bypass was added at the fault sites.

## Reference contracts

Existing Kyty/Jetsku event flag and pthread sources were inspected. Jetsku publishes
its pthread object before starting execution, matching the ordering required here.
Secure CRT semantics for successful calls and tested error cases use these references:

- https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/memcpy-s-wmemcpy-s?view=msvc-170
- https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/memmove-s-wmemmove-s?view=msvc-170
- https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/strncat-s-strncat-s-l-wcsncat-s-wcsncat-s-l-mbsncat-s-mbsncat-s-l?view=msvc-170

These host CRT references do not establish every PS5 error ABI or invalid-parameter
callback behavior. The captured successful guest calls are the integration evidence.

## Append verification

`astro_local_strncat_s_log.txt` confirms actual appended graphics, skeleton and
animation paths at T+88.04..88.08. The game opens
`/app0/data/prein/effects/anim/hero_respawn_icon_01.skel` successfully (fd 168)
at T+88.21. The prior null Animation crash has not recurred at that stage.
Vulkan proceeds into compute shader translation and `TileBasedLighting` markers.
This is resource/render progress; no visible frame is claimed from it.

## CPU copy notification correction

The append run reached three GPU flips, then exited with code 321 at
`BufferCache::PublishImageBacking`: a 16-byte secure copy was already CPU-dirty
inside a cached buffer. The new copy handler incorrectly called
`PsemuMarkCpuModified`, whose implementation actually performs image-backing
ownership publication and requires clean buffer owners. Removed this extra call;
secure copies now use the same writable-page checks and tracking route as the
existing ordinary memcpy/memmove handlers. Image ownership assertions remain intact.
All three regressions and local full loader link passed again. Follow-up game run:
`tools/data/astro_local_copy_tracking_log.txt` (verification still in progress).

`images/run_20261014_032505/frame_00000.bmp` contains a striped color pattern,
not recognizable game content. Completed draw coverage/readbacks still report zero
covered samples/zero RGB for measured targets. A nonblack capture of an unwritten
presentation image is not evidence of correct rendering. No first game frame achieved.

## Positioned reads (next failure)

The copy-tracking follow-up failed earlier during parallel resource loading at
RVA 0x705147f (null nested payload); exit 0xc0000005. This does not validate the
GPU-copy correction past the prior assertion yet. The preceding log reveals
`sceKernelPread` of real .szd resources returning zero through the default stub.
Implemented positioned binary reads under the existing VFS mutex, preserving the
shared file cursor across seek/read/restore. SysV offset is RCX; kernel errors use
negative 0x800200xx codes, while libc pread converts errors to -1 plus guest errno.
This is an independently missing API; its causality for the new null read is not
claimed until runtime evidence. Kyty and Jetsku KernelPread use the same
save-position / seek / read / restore approach under a file mutex.

New positioned-read regression verifies embedded zero bytes, guard bytes, unchanged
cursor, short read, >4GB offset without allocating a large file, invalid offset,
bad descriptor and invalid buffer. It and the previous three suites pass locally;
full local loader build also passes. Runtime log: astro_local_positioned_read_log.txt.

## Event flag dispatch

The positioned-read run read the real master audio bus .szd (4951/4951 bytes,
T+67.53), then its log stopped after T+81.87. Sampled guest worker RIP
0xdf26aa checks its own wrapper flags and skips the event wait when the stored
handle is null. `sceKernelCreateEventFlag` was returning success without writing
that handle. Connected create/wait/poll/set/clear/cancel/delete to the existing
Kyty kernel event flag implementation (already built into psemu). HLE validates
output spans and wait modes; no guest instruction skip or synthetic signal added.

`loader.exe --event-flag-self-test` exercises a real worker wait/wakeup, AND/OR
poll modes, bit clearing, timeout, cancel and delete without loading a game or
creating a GPU. Passed locally; added it to the Windows workflow. Full local build
passed. Runtime verification: tools/data/astro_local_event_flag_log.txt.
The earlier stalled owned game was stopped after executable-path verification;
its exit -1 is intentional termination, not a new measured crash.

## Material substring lookup and direct copy imports

Event-flag run progressed past the initial quiet interval: a second real .szd read
completed (21363/21363 at T+129.93), then loaded actual graphics/skeleton/animation
resources through T+322.49. Event flag output and set calls execute; this is not a
verified permanent stall. The process ultimately exited 0xc0000005 at RVA
0x74e3242: immediately after PLT#1570 `strnstr`, the guest reads returned_pointer+7.
The import had no implementation and returned zero. The captured needle is
`target\data`. Added a bounded substring search returning an offset into the
original guest string; no guest crash-site branch was bypassed. Regression covers
bounded fit/exclusion, case sensitivity, zero length, empty needle, embedded NUL,
and a source without a terminator inside its bounded span.

Sampled hot audio/resource worker stacks contain repeated secure-copy calls.
`memcpy_s`/`memmove_s` now share one validated routine between HLE fallback and
direct SysV imports. No copy/error/ownership semantics change in the fast path.
Actual loader --secure-copy-self-test validates SysV argument passing, binary copy,
overlap, range/null errors, guest errno, invalid pointers, and a read-only page.
It and native event flag tests pass; full local build passes. Startup speed and
strnstr integration remain pending in astro_local_native_secure_strnstr_log.txt.
No first recognizable Astro frame has been achieved.

## AudioOut2 context metadata and queue model

Implemented the existing Kyty 64-byte context parameter defaults, memory query,
context handles and time-based queue advance/push/level/destroy operations. The
queue model uses the requested grain count at 48 kHz; full nonblocking push returns
Kyty's 0x80268008 NOT_READY code. Queue-state locking does not span the blocking
sleep. This is context/queue support, not an implemented PCM device, AudioOut2
port backend, ACM batch engine or complete PS5 error ABI.

Queue regression covers full/available states, precise grain boundary consumption,
partial elapsed grains, repush after emptying, backward time, zero-default fields
and overflow-safe large grain arithmetic. It passes; local loader build and direct
secure-copy/event-flag self-tests pass. Actual game log
astro_local_audio_queue_probe_log.txt confirms memory size 66960, context handle1,
and queued1/available0 for the game's one-entry queue after push. Real .szd reads
still return all requested bytes. Material lookup succeeds at T+297.51.

The previous uninstrumented run passed four GPU flips without the earlier
image-ownership assertion. It was intentionally stopped for the next local build.
The new Mesh output trace still records dummy allocation1/1, packed indices0/0/0,
position(0,0,0,0), and empty-input gate PC0x104 value0 for instance0. Measured color
readbacks and coverage remain zero. Four presentations have occurred, but the
captured striped pattern remains invalid game-frame evidence. Next distinguish
instance0 from the other actual instances in the 512-instance Mesh draw.

## Selected instance trace extension

Added opt-in PSEMU_ASTRO_MESH_PROBE_INSTANCE (default0) to select one actual
instance. The host binds the trace only for one-point draws whose instance range
contains the selector. Gate/header/position/index trace stores are guarded by the
instance in the existing five-word push payload. The other instances keep executing
their original shaders and render outputs; only diagnostic SSBO stores are omitted.
The fault header layout remains intact. Changing selection requires a fresh run/cache.

Draw-selector tests cover actual512 range, exclusions, singleton selection,
wrapping u32 IDs, zero instances and multi-point rejection. Native Mesh emitter
regressions and Vulkan1.2 spirv-val pass, including a new point-payload trace variant
through the existing internal wave64 adapter. Full local Windows build passes.
Arc B580 control smoke passes exact3 positions, packed(0,1,2), allocation3/1,
fault header preservation and1024/1024 correct red test pixels; log
local_selected_mesh_gpu_log.txt. This is explicitly a control, not Astro game output.

Actual instance1 run started 2026-10-14 at13:06+03:00, owned PID5912;
log astro_local_selected_mesh1_log.txt. Runtime result is pending.
The preceding interrupted-run process was absent on this next turn. Its monitor
left no exit JSON, and its last log established no fatal cause; do not call that
absence a verified new crash. No recognizable Astro frame has been achieved.

## Dynamic point-payload GPU verification

Extended mesh_gpu_smoke with point-output-probe and point-output-miss modes.
Both use the same internal wave64 merged point fixture and actual five-word
push payload. Arc B580 passes selected instance0 exact geometry/gate trace,
and nonselected instance1 leaves the entire trace buffer untouched. Both
render1024/1024 correct red control pixels, verifying that trace selection
does not suppress geometry. Log local_point_selection_gpu_log.txt.
These are control fixtures, not recognizable Astro game-frame evidence.

## Actual instance1 result and descriptor trace

Owned PID5912 completed the actual512-instance Mesh draw. Instance1 reaches
PC0x104 with record1 and value0; allocation1/1, indices(0,0,0), clip(0,0,0,0).
The primitive query counts512 generated primitives with zero covered samples.
This rules out instance0 alone accounting for the sampled empty-input branch;
it does not prove all512 input records are empty. Later fullscreen draws do
cover their targets, but measured RGB readbacks remain zero. The process was
intentionally stopped to replace our diagnostic executable (exit-1).

Extended the opt-in lane trace from10 to17 words to capture scalar descriptor
s8:s11 and record xyz v5:v7 immediately before CMPX. Existing fault-header
words and first10 lane fields retain their meaning. No guest inputs change.
Vulkan1.2 validation, full local Windows build, and Arc B580 selected/miss
control tests pass. Known descriptor words11111111/22222222/33333333/44444444
and record X equal to lane ID are captured exactly; the miss buffer stays zero.
Log local_input_sharp_verified_log.txt. Actual descriptor runtime is pending.

## Actual descriptor and presentation capture correction

Instance1 GPU sharp is raw9fb86010/005001f8/00020000/00005204,
base0x000001f89fb86010, stride80, records131072 (10MiB). Its xyz and gate
W are all zero across observed repeated draws. DMA also accesses a counter
at base-16. This may be an inactive effect pool, not proof of the whole scene
being empty. Do not force this gate positive or call it the established cause.

Astro registers video format0x8100000000000000, decoded by existing policy
to A2R10G10B10UnormPack32. Screenshot previously interpreted packed10-bit
bytes as BGRA8. Corrected BMP and scene-signature channels using rounded
10-to8-bit conversion for both packed channel orders. Existing8-bit paths
remain unchanged; unsupported formats are explicitly skipped instead of
using an incorrect4-byte footprint. Capture every initial8 presentations,
then every4th, retaining distinct-scene and60-shot bounds. Capture pixel
regressions cover channel order, black/white, midpoint, and all alpha bits.
Tests and local Windows build pass. This changes diagnostic readback only,
not shader outputs or presentation. Prior striped BMPs are invalid evidence.

## Correct capture runtime and missing HDR query

Corrected run PID8484 produces format58 (A2R10G10B10UnormPack32) capture
images/run_20261014_134056/frame_00000.bmp. It is nearly black with a faint
pattern, not a recognizable Astro frame. Measured intermediate RGB remains
zero; repeated selected Mesh record stays empty. Four flips are observed.
The process was intentionally stopped to replace our diagnostic executable.

Actual Astro invokes sceSystemServiceGetHdrToneMapLuminance at startup and
every observed frame. Loader dispatch previously returned success without
writing its12-byte output. Connected this path using shared typed defaults
80/1000/0 from existing Kyty SystemService source. The existing library
implementation also uses this same helper. Full guest span is validated;
invalid output returns SystemService parameter error0x80a10003.
No claim yet that this query explains all black geometry/targets.

Local Windows build and --hdr-tone-map-self-test pass (canaries, exact12-byte
output, allthree luminance values, null/invalidpointer error). Event-flag and
native secure-copy regression self-tests also pass. Runtime result pending.

## Dispatch audit correction

sceVideoOutGetOutputStatus already reaches registered native NIDutPrVdxio-8
through the earlier Agc::Dispatch path. The added later core branch is unreachable
in the normal game call and was unnecessary. Reverted2714b11, retaining HDR
query correction and packed10-bit capture correction. Do not report output
status as a missing implementation or as a game-progress fix.

The active combined-run binary contains that unreachable extra branch, so
normal game behavior is equivalent to the corrected source for this call.
Owned PID5180 continues; no need to restart it just for this source cleanup.
Actual HDR output80/1000/0 has been confirmed in game. No recognizable frame yet.

## Longer boot and missing CPU float calls

HDR output is confirmed in real repeated game queries. Intermediate RGB
readbacks still zero; no recognizable frame. Let the run continue beyond the
first4 presentations: prein.xml processing atT570/605/642/673, Transition.xml
atT710, then HTTP empty-response fallback atT745. The game is progressing
its startup work even though render content is still black. Working set6.7GiB
at longer run; process was intentionally stopped for the next actual fix.

Observed exp2f and atanf imports had neither native PLT mapping nor HLE float
output handling, and are not registered in the GPU library fallback. Old stub
returns integerzero without computing XMM0. Added real SysV unary wrappers
and HLE XMM0 copyback. --unary-float-self-test verifies XMM0 upper bits/flags,
zero/negative/infinity/NaN and volatile SysV function pointers, including
exp2f(0)=1, exp2f(-3)=0.125, atanf(1)=pi/4. Bounded real input/output log.
No claim yet that these two calls alone explain missing scene geometry.

Initial8 capture images are now saved unconditionally, retaining every4th
subsequent readback and60-shot limit. This prevents small visible logos/text
from being hidden by a sparse scene-signature threshold. All artifacts onD.

## Actual float proof and later sampled alias crash

Real Astro now calls exp2f(16)=65536, exp2f(0)=1 and atanf(1)=0.785398185.
Guest callerRVA0x1100f6e uses the exp2 result for a power-of-two table capacity
(stored fields+0x78/+0x80), so the old unchanged exponent was not valid output.
The four initial format58 captures remain identical/nearly black, no game frame.
Boot passes Resident load, prein.xml and Transition.xml, then opens plaza camera
animations atT701. Normal-runtime failure (exit321) occursT~702 at textureCache
unsupported GPU invalidation alias, not a CPU access violation.

Captured write0x2a3b9180000+0x3c0000 ends0x2a3b9540000; sampled image
0x2a3b9500000+0x800000 overlaps only first0x40000. Native image clean,
buffer_modified1, formatted1, ambiguous0. Added only already-buffer-owned
sampled-view invalidation for byte-overlapping page-aligned formatted writes
that cross the image edge. No native GPU bytes are discarded/downloaded;
sampling uses its existing dirty-buffer refresh path. Native-image owners,
unaligned/unformatted writes, absent buffer ownership and ambiguous aliases
remain rejected. Original contained-write behavior remains unchanged.
Reference Jetsku's ownership-aware invalidation/download serial model informed
review; the local existing yielded-buffer path is retained instead of copying
incompatible cache machinery. New bounded BUFFER-OWNED-TEXTURE-WRITE log.

Captured-range classification regressions plus both edges, owner conflicts,
alignment, format, disjoint boundaries and original contained writes pass.
All existing depth-storage roundtrip/fragment preservation tests and full30-step
Windows build pass. Logs local_sampled_crossing_tests/build_log.txt.
Actual rerun across this crash is pending.

## Longer post-alias boot and real SPX directory failure

Run PID9920 passes the earlier plaza-camera load stage, logs Armadillo Load
end atT723.76 and opens astro.jxm/astro.skel atT729. No recognized game frame;
first8 format58 captures remain near-black. Exact crossing-write acceptance
marker was not observed, so do not claim the captured ownership case has
been pinned in runtime merely from elapsed-time/boot progress. Process was
intentionally stopped for the next filesystem fix.

Game opens /app0/data/prein/spx and gets ENOENT, but that directory exists
and contains real SPX files (aerial_aquarium.spx etc). Core KernelOpen treated
directories as FILE* and fopen fails. Added separate directory snapshots in
the same monotonically allocated fd namespace, never mixing native registries.
Open enumerates actual UTF8 names/types, applies existing Common::hash inode
policy, packs Kyty FreeBSD dirents in512-byte blocks. Getdents/getdirentries
return bytes/cursor/base offset; close releases snapshots; directory fstat
reports directory type and packed size. KernelRead shares the same snapshot
cursor. No generated game data or asset patching. Original regular-file path
retained. Native registered SDK database only owns graphics/font/pad/fiber,
so directory dispatch is not another unreachable VideoOut-style branch.

Reference existing gpu/src/kernel/fileSystem.cpp KernelGetdirentries and
original Kyty PackDirents implementation. Package regression validates long
name255, mixed types, duplicate-free multi-block traversal, null/short output,
EOF, rejected overlength input with unchanged output and empty directories.
Loader integration reads actual test files/directories, verifies canaries and
invalid/closed descriptors. Tests, full Windows build, secure-copy and event-flag
self-tests pass. Log local_directory_tests_log.txt. Actual SPX enumeration pending.

## Directory run blocked before its actual directory call

Owned PID5480 exits321 aroundT729 before SPX access: unsupported storage-image
byte alias in textureCache.cpp. Requested0x19c69780000+0x3c0000 is1216x684
pitch1280 tile24 fmt22 (single-depth/single-mip); old RenderTarget kind2
0x19c696f0000+0xc0000 is480x270 pitch512 tile27 bpe4 (single-layer/mip).
The ranges cross by0x30000. Both cached/tracker GPU flags0, buffer flag0,
same context1, CPUdirty0 and no resident buffer over requested range.
No actual SPX enumeration has yet been proved by that run; retain pending status.

Added a separate clean RT/storage edge-retirement predicate; the existing
containment and RT/RT rules remain unchanged. It requires actual byte-crossing,
valid aligned nonwrapping ranges, same context, coherent requested backing,
no cached/tracker GPU owner, no yielded buffer, no GPU-dirty buffer anywhere
in the old range, and single-mip/layer/depth/base-level0 2D storage. It uses
existing RequireRetirementIsolation and RetainResourceUntilFence, without
writing backing or inventing pixels. RT-STORAGE-EDGE-RETIRE measures acceptance.
Captured/both-edge policy regressions and negative owner/context/backing/shape/
alignment/adjacency/overflow cases pass. Existing depth-storage data roundtrips
and fragment preservation tests pass. Full30-step Windows build passes and
actual directory integration self-test still passes. Logs local_clean_storage_edge
_tests/build_log.txt. Runtime proof and first recognized game frame remain pending.


## 2026-10-27: remove unused native physical-pool commit charge

Fresh runs failed all large primary direct-memory maps with Windows error1455,
then passed null video buffers into the correctly rejecting video-out guard.
At the first failure only ~350-800MiB system commit remained, despite process
private bytes being ~1.75GiB. The native Kyty physical backing constructor was
implicitly SEC_COMMIT for its entire13.5GiB section. psemu already uses a separate
primary physical backing; creating the unused native section exhausted commit.

Native DirectMemoryBacking now creates SEC_RESERVE and commits only physical
spans mapped through MapView/ReplacePlaceholderView. Recommit preserves data
and does not change guest-view protection. Out-of-range/failing commits remain
errors. Primary allocation/map failures log actual Windows errors and available
commit; no system/pagefile configuration or game assets were changed.

--native-backing-self-test verifies the actual13.5GiB section begins reserved,
only mapped pages become committed, the final untouched page remains reserved,
and NOACCESS plus writable aliases remain coherent. Primary backing regression
also verifies repeated commit preserves bytes. Full local build, native backing,
shared backing, secure-copy ABI and actual-directory integration tests pass.

Runtime astro_20261027_sparse_backing maps all six initial primary ranges,
including0xc8000000,0x87400000 and0x120000000 bytes; no DIRECT-COMMIT-FAIL remains.
It reaches frame4 / vkCreateComputePipelines for258230 SPIR-V words before a
0xe06d7363 C++ notification and truncated allocation-heavy VEH diagnostics.
This proves the early map failure is fixed; no recognized frame is claimed.

MSVC first-chance C++ exceptions now continue normal exception search before
stringstream/PLT crash logging, so driver-internal catches can operate without
recursive diagnostic allocations. Actual escaping std::exception at compute
creation logs what(), shader length and Windows available commit/RAM, then
rethrows without replacing the shader or fabricating a successful pipeline.
Vulkan mesh control regression passes1024/1024 pixels on Arc B580; this is a
control fixture, not game output. Owned retry astro_20261027_compute_exception
is running; actual SPX enumeration and first recognized game frame are pending.

## 2026-10-27: compute initialization failure at system commit exhaustion

Retry astro_20261027_compute_exception exits321 at13:04, after successful
258230-word and278800-word compute pipelines. The293827-word shader returns
vk::Result::eErrorInitializationFailed. No escaping std::exception is recorded.
The monitor records5694MiB private /5058MiB resident and32334/32691MiB system
commit at13:03:51; a new PowerShell process fails with OutOfMemoryException.
The game had already exited before the attempted owned-process stop, so this
is an actual exit, not an intentionally terminated test. Commit returns to
~17.3GB after exit. First-chance C++ bypass allowed the earlier shader to finish,
but does not by itself solve the measured global memory limit.

Added opt-in PSEMU_DMEM_FILE_BACKED=1 for the Astro primary physical pool.
TEMP must resolve to an existing D: directory. The new scratch file uses
CREATE_NEW, FSCTL_SET_SPARSE and DELETE_ON_CLOSE, and shares actual file pages
between guest/backing views. All unallocated spans start NOACCESS in both
views. Commit enables only newly allocated pages; recommitting preserves data
and previously established NOACCESS/READONLY guest tracking guards. This also
fixes the pagefile Commit wrapper to avoid resetting existing guest guards.
Backing read/write retains actual range and protection validation. No pagefile
or other Windows settings, C: files, or game assets are changed. Normal mode
and Dreaming Sarah retain their existing backing selection.

New test_shared_direct_memory.ps1 compiles and runs both actual backends,
including16GiB sparse file /10GiB logical allocation with <512MiB commit charge,
zero-initialized pages, alias coherence, overlapping recommit that preserves
old guards, invalid ranges, C: scratch rejection, and delete-on-close cleanup.
Full local build, both backing regressions, native backing and directory HLE
self-tests pass. The actual retry astro_20261027_file_backing reports D-file
ready=1 and all six large maps succeed. At~T90 system commit is~20.15GB instead
of~30GB; Vulkan compilation and recognized game output remain pending.
## 2026-10-27: D-backed run passes the failing compute and real SPX enumeration

Owned PID19444 / astro_20261027_file_backing reaches presentation successfully.
The previously failing293827-word compute shader succeeds and the real draw
pipeline continues. First six presentations remain visually black/dithered
(diff=0), confirmed by viewing frame_00000.bmp in images/run_20261027_131603.
Render-target nonzero data alone is not recognizable game output.

Unlike the prior pending status, the actual game now opens data/prein/spx,
enumerates411 entries /13824 dirent bytes (fd793 andfd1298), and opens the real
SPX files, including world_map_selection_top.spx and zoo.spx. Armadillo Load end
is logged atT834 and Transition Load end atT925. This is runtime proof of the
directory fix; no generated assets or fabricated directory entries were used.
AtT~974 the process remains alive while loading additional character/effect
resources, ~7.2GiB private and ~25GB system commit /30GB limit. Runtime memory
samples are in astro_20261027_file_backing_memory.csv. First recognized game
frame and subsequent pipeline correctness remain unresolved.
## 2026-10-27: opt-in pre-entry Astro TLS preparation

D-backed runPID19444 was intentionally stopped at~T2004 after >3700 actual
source opens. It remained alive, but its first eight framebuffer captures were
black/dithered. Thread samples resolve hot VirtualQuery callers to SafeReadable
in the FS-load fault decoder. Existing log explicitly says fast TLS is disabled;
read-only live counters measure~5810 core VirtualQuery calls/s and~770 wait/signal
semaphore pairs/s. The historical dynamic TLS patch is known to race, so it was
not enabled on the running process.

Offline analysis uses this build's real PT_GNU_EH_FRAME header (312697 sorted
function entries), then follows reachable direct CFG edges within each candidate
function. 3630 raw FS0 candidates occur in3087 functions;3553 are reached as
complete instructions, all exact12-byte padded mov rax,fs:[0]. Unknown indirect
edges and unmatched sites are deliberately left to VEH. Each prepared site
requires both the full instruction and8-byte contexts before/after it to match.
No runtime cross-thread code mutation is used for this mode.

PSEMU_ASTRO_STATIC_TLS=1 opts into allocating/verifying the existing Windows
TEB slot and preparing the verified sites before game execution. All newly
created guest threads retain the existing thread-local slot initialization.
Astro dynamic TryPatchFsMov is disabled in this mode; unknown FS instructions
continue the interpreter/fault path. Sarah behavior is unchanged.

New actual-machine-code regression executes the patched GS load on8 threads
with distinct values (1000 reads each), validates slot bounds and all28 single-
byte context mutations fail without modifying output. Full local build passes.
Owned retryPID18760 / astro_20261027_static_tls confirms3553 prepared sites and
verified slot15/offset0x14f8 before guest threads, and passes the first minute
without the historical early dynamic-patch crash. Later runtime speed, stability
and first recognized frame remain pending; this mode remains opt-in.
The reproducible generate_astro_tls_sites.py --check also passes using Capstone
instruction groups for conditional/unconditional branches, returns and indirect
edges, yielding exactly the same3553 sites. It requires the pinned plaintext
SELF unwind layout and emits no game assets. Runtime static-TLS retry reaches
all initial compute pipelines, including293827/310885 SPIR-V words, successfully
at~T375 without the historical early patch crash. First presentation still pending.
Static-TLS runtime follow-up at14:00: PID18760 remains alive pastT1092 and
>2300 actual resource opens; no new crash. Runtime PERF explicitly measures
tls0/s. It opened eff_hero_death_firetrap_fire.jpx atT961.37 versusT1097.84
in the prior D-backed run (specific-resource comparison, not a general FPS
claim). First eight presents remain identical black/dithered (diff0); first
recognized game frame is not achieved. A sampled GPU engine counter reports
~1%3D at13:54, so43s frame time cannot be attributed solely to GPU execution
from elapsed time. High semaphore/VEH CPU traffic and pending source loading
need further measurement. Owned test remains running; root Sarah loader and
user launcher edits were not overwritten. Latest code commit27ec070.

## 2026-10-27: mutex cache collision handling and native trylock

Continued owned static-TLS runPID18760 is alive pastT1649 and4698 resource
opens. No recognized image yet; first eight captures remain black/dithered.
Thread sampling atT~1200 finds hottest thread18980 in SafeWritable called from
GetOrCreateMutex (exact map offsets0x37890/0x37a3b). Worker threads mostly wait
on real semaphores. Live counters separately show~9061 core VirtualQuery/s,
~4498 wait/signal pairs/s. Call counts alone do not prove semaphores are the
main CPU cost; their existing blocking semantics have not been weakened.

Replace the16-entry direct-mapped TLS mutex cache with64 four-way buckets,
folding address bits so256-byte-strided slots do not repeatedly evict each
other. Same pointer AND unchanged handle are still required for hits. Misses
retain SafeWritable, synchronized registry lookup and existing real critical
section creation. No mutex ownership or destroy/attribute semantics changed.
Add direct SysV scePthreadMutexTrylock/pthread_mutex_trylock using exactly the
existing TryEnterCriticalSection/EBUSY16 behavior, as lock/unlock already do.

Production-extracted regression uses32 slots that collide in the original
cache, then32000 warm accesses with zero extra VirtualQuery. Also verifies
slot-content reuse, real contention/busy then successful acquisition,8000
protected updates across8 threads and unchanged NOACCESS rejection. Local full
build and native-backing/secure-copy/directory tests pass. Candidate executable
is D:/Tools/psemu-build/mutex-output/loader.exe with its own loader.map; the
still-running PID18760 uses the older output/loader.exe. No game-runtime FPS
improvement from this candidate has yet been measured. build-local/loader.map
now belongs to the candidate and must not be used to symbolize the old PID.

## 2026-10-27: wire existing Kyty offline HTTP into actual SDK dispatch

Actual Astro calls sceHttpCreateRequestWithURL/SendRequest/WaitRequest/
GetStatusCode/GetResponseContentLength, but no HTTP handler was registered in
core or native SDK tables. Core generic success left outputs untouched; the
runtime status log contains a pointer-like2370821947392. This proves an output
contract gap, not that HTTP alone explains the black framebuffer.

The local existing Kyty network.cpp already has typed pool/SSL/HTTP/template/
connection/request IDs, before-send/timeout/invalid-ID results and zeroed status/
length outputs. Compile and reuse that implementation (same Jetsku-style offline
behavior). PSEMU_NATIVE_HTTP=1 registers only HTTP34 functions, SSL4 and pool2
with their original libNet NIDs in both native SDK tables before first lookup.
It does not register sockets/PSN or implement successful Internet responses.
Actual socket-fd helper definitions replace the old stubs because network.cpp
now owns those symbols; no socket can be created through this opt-in table.
Windows dependency libraries ws2_32/iphlpapi are linked from the existing D SDK.

--http-self-test sets its CRT-local flag before lookup and verifies actual NID
pointers, real ID lifecycle, pre-send error, offline timeout, exact bounded
status0/content-length0 outputs and canaries, empty nonblocking poll with
unchanged event memory, deleted-ID rejection and cleanup. It makes no network
request. Test and full10-step local build pass. Native-backing, secure-copy and
actual-directory self-tests pass with HTTP flag disabled. New candidate remains
in D:/Tools/psemu-build/mutex-output; normal default remains opt-in.

Old owned runPID18760 reaches ui_pause_next LoadLevelResources complete atT2207
(report997.230s), then opens ui_pause_hub/level.lvx atT2295/fd5409. It has no new
recognized frame or fatal error; identical output makes a controlled restart
with the tested mutex+HTTP candidate appropriate. Native HTTP runtime acceptance
and first recognized image remain pending; do not mark these complete from the
standalone test. Existing Sarah executable/launchers remain untouched.
## 2026-10-27: correct SysV printf integer width and star arguments

Real HTTP runPID22308 logs sceHttpSendRequest error atT227.58 with low32 bits
0x80431068, then progresses through prein.xml andTransition.xml. This confirms
the offline error path is accepted by the game. Its pointer-like upper digits
are a separate formatter bug: FormatSysVPrintf normalized every integer to
64 bits without truncating promoted32-bit int values from64-bit GP save slots.
The same formatter supplies vsprintf_s resource identities, so this affects
names as well as diagnostics. The earlier pointer-like status display alone
was not proof of uninitialized SDK outputs; the missing HTTP output contract
was independently established from the absent dispatch implementation.

Integer conversions now apply signed/unsigned32-bit promotion,8/16-bit h/hh
narrowing, and retain LP64 long/long-long/size_t/intmax_t/ptrdiff_t on Windows.
Dynamic width/precision arguments are passed to snprintf with their actual int
width instead of being discarded while leaving stars in the host format.
Narrow strings obey precision/width after a bounded guest read and preserve
longer-than1024 content using correctly sized output. Existing unsupported
wide-string fallback is unchanged; this is not a claim of full printf coverage.

Production-extracted test_secure_format now poisons upper GP bits, checks
__UNKNOWN__0, exact80431068 hex, sign extension, narrow types, LP64 types,
register/stack overflow arguments, negative dynamic width, dynamic FP/string
precision,2048-byte strings and existing canary/readonly/short-buffer checks.
It passes at/Od (same core optimization policy). Full local build in
D:/Tools/psemu-build/format-output passes HTTP, secure-copy, native backing and
actual-directory self-tests. Mutex production regression also still passes.

On resume at17:28, no loader process remained and the prior run has no exit
JSON or conclusive fatal diagnostic; do not report a proved crash. Last actual
file progress reachesT1601/fd4312 hero_scarf resources; captures stay black.
Color-mask hypothesis rejected: pipeline write masks use render-target mask
through export_mapping, not the logged zero CB_SHADER_MASK. Clock-zero suspicion
also rejected: graphicsRun.cpp writes ReadReferenceClock() before recording the
sync diagnostic. CPU/GPU label ordering still needs a separate audit against
Kyty TryDeferLabel; no speculative synchronization change has been applied.
## 2026-10-27: native counted semaphore dispatch, opt-in

Format retryPID22404 confirms actual HTTP error now prints exactly80431068 at
T226.68; no new fatal diagnostic. First capture is still black. Read-only
snapshot validates image timestamp/map/owned executable, then reads the writable
physical backing alias for the actual GS descriptor0x1aa99856010, stride80,
131072 records /10MiB. All CPU records, including offset44 gate values, are zero;
selected GPU record1 probe also reads zeros. CPU/GPU ownership/coherency has NOT
been established, so this is not proof of a shader or game-side initialization
root cause. No complete captured CS buffer binding in the prior1097-bindings
log overlaps its descriptor range; incomplete/indirect producers remain possible.
GPU process memory ~1.23GB dedicated +3.19GB shared rules out filling12GB VRAM
in that sample, not every possible bandwidth or paging issue.

Live counters in previous candidate show~9451 core VirtualQuery/s and~4666
wait/signal pairs/s; the mutex-cache test improvement did not establish an
end-to-end frame-speed improvement. Add PSEMU_NATIVE_SEMA=1 to register all6
sceKernel semaphore NIDs together in both SDK tables. This reuses the existing
Kyty kernel/semaphore.cpp counted, ordered waiting/cancellation implementation,
not a fake-success bypass. Private unnamed semaphores retain existing psemu
compatibility by passing an empty debug label to the real primitive. Every
create still returns a real independent semaphore. Counts, actual blocking,
remaining timeout, overflow and cancellation are now native. Existing default
remains off; changing backend requires a fresh process, never mixed WinHANDLEs
and native pointers. Old core poll-empty returnedESRCH; native returnsEBUSY.

--kernel-semaphore-self-test validates real SysV/NID pointers, unnamed create,
failed multi-count poll leaves count intact, blocked need2 remains blocked after
one signal, second signal actually releases it, timeout writes0, overflow and
invalid need errors, cancellation reports the pending waiter and sets count3,
and real cleanup. Full local build and semaphore, HTTP, native-backing and
secure-copy tests pass. Candidate is D:/Tools/psemu-build/sema-output/loader.exe
with its matching loader.map. Native game runtime/performance and first visible
frame remain pending. Shader mask/clock/label hypotheses are not claimed fixed.
## 2026-10-27: unify guest counter imports with VideoOut's native clock domain

Native-semaphore retryPID6292 accepts real hkSemaphore objects (init0/max1).
GPU-stage live core VirtualQuery sampling drops to~44.7/s from prior~9451/s,
with no wait/signal PLT trap counters in the sample. This is a dispatch-load
result, NOT a measured FPS gain: first capture360s, subsequent captures~46-60s
apart, still black. prein.xml steps464.46/464.80/465.09 andTransition465.37 occur
earlier than prior~659s, but scene/image correctness remains unresolved.

Source audit establishes a separate cross-interface clock inconsistency:
Core's sceKernelReadTsc/GetProcessTimeCounter return absolute QPC ticks and
frequency10MHz; native VideoOut VblankBegin/VblankEnd/flip statuses use Kyty
KernelGetProcessTimeCounter (process-relative TSC), KernelGetProcessTime and
KernelReadTsc (raw TSC). Their native frequency is3.7GHz on this host. A guest
interpreting VideoOut processTimeCounter using its imported frequency therefore
mixes both units and epoch, even though Core's QPC pair is internally consistent.
This is NOT proof yet that it causes the minute-long frame interval.

PSEMU_NATIVE_CLOCK=1 registers all5 corresponding clock NIDs in both SDK tables
to the exact existing Kyty functions used by VideoOut. No GPU reference-clock
scaling, FPS forcing, timestamp fabrication, or Windows clock setting changes.
Counter, frequency and microsecond outputs now share native domain by construction;
POSIX clock_gettime/gettimeofday remain their existing separate interfaces.
--clock-self-test verifies actual NID/SysV pointer identity, native frequency,
monotonic tick/microsecond deltas against QPC wall duration, and common epoch
with native VideoOut-producing functions. It reports native_hz3700000000 vs
qpc_hz10000000. Full local build and clock/semaphore/HTTP/backing tests pass.
Candidate D:/Tools/psemu-build/clock-output includes its matching loader.map.
Runtime effect on frame time and first recognized game frame remain pending.
## 2026-10-27: direct, bounded native character searches

Clock candidate 8426dc0 runs through actual Vulkan submission. Captures at
18:17:23 and18:18:25 are still black/dither (~62s interval); no first-image or
frame-speed success is claimed. Native clock coherence fixes a real interface
inconsistency, but did not remove this observed long frame interval.

A read-only live sample during parsing reports ~6248 strchr/s,6247 strrchr/s,
2107 sscanf_s/s and32374 core VirtualQuery/s. Existing searches first scan and
copy the entire remaining NUL-terminated XML suffix before searching, and still
trap through VEH. Add NativeStrchr/NativeStrrchr to the existing native PLT
selection. GuestFindCharacter queries each current readable region, scans in
<=4096-byte chunks, stops first-search at its first match, and does not allocate
or copy the remaining document. It does not cache page permissions. Reverse
search retains the final match, NUL searches require an actual terminator, and
int search values are narrowed to unsigned char as required by libc. Native
PLT disabled retains the existing VEH fallback. Invalid input handling is
bounded by current mappings; concurrent unmapping retains existing HLE lifetime
requirements, with no new claim of transactional memory access.

Production-extracted /Od SysV pointer tests compare CRT semantics, byte
truncation, empty strings, adjacent readonly regions, NOACCESS/GUARD boundaries,
unterminated input and a2MiB suffix with10000 early searches. Tests pass; full
local build, secure-format and clock/semaphore/HTTP self-tests also pass.
Candidate D:/Tools/psemu-build/search-output includes matching loader.map.
Next runtime disables heavy per-draw pixel/coverage/mesh-output probes while
retaining actual framebuffer captures; instrumentation cost and search effect
will therefore not be attributed separately without a controlled comparison.

## 2026-10-27: avoid full mapped-region walks in small HLE checks

Search-only candidate a08462b, PID19484, first captures18:25:58/18:26:55/
18:27:37 are still black, with57/42s intervals. Removing heavy per-draw probes
and direct searches has not established a visible-frame or major FPS success.
Read-only thread samples show both hot CPU threads in NtQueryVirtualMemory
through SafeReadable/SafeWritable/SafeReadCString. An external10-query sample
at actual primary pool address0x2205ad02c5f reports region0xdab5e000 and mean
VirtualQueryEx60.10ms(min46.69/max69.22); small heap region query~0.0025ms.
Current primary mapping is D-file backed with large protection-expanded ranges.

GuestMemoryQuery::Query uses K32QueryWorkingSetEx for a VALID resident page's
current Win32Protection, returning only that one host page's extent. Invalid
working-set metadata is never interpreted; unknown/nonresident/reserved/free
entries fall back to VirtualQuery. No permission cache, stale permission reuse,
page touching, guard consumption, memory mapping or pagefile setting changes.
This follows Microsoft PSAPI_WORKING_SET_EX_BLOCK Valid/Win32Protection contract:
https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-psapi_working_set_ex_block
Applied to the measured SafeReadable/Writable/Strlen and native character-search
helpers. g_vq_calls counts actual fallback VirtualQuery calls; resident queries
are not counted there. Page-size-clipped regions remain valid for all4 callers.
The existing concurrent-unmapping lifetime limitation remains unchanged.

A real1GiB sparse D-file with whole-range NOACCESS->RW protection reproduces
slow region walking: VirtualQuery7.419ms vs page query0.003ms in test. No timing
assertion is used. Tests verify actual resident fast path without fallback,
nonresident/reserved/free fallback, current RW->RO->NOACCESS->GUARD changes,
guard retained, independent alias permissions and delete-on-close cleanup.
Character search, secure format, mutex-cache, native atomics and semaphore
handler regressions pass. Existing file-I/O source-extraction harness had stale
HDR/directory/positioned-read dependencies; updated it to use actual production
reader/globals and verified its real open/write/read/stat/canary/close test.
Full local build and clock, native-semaphore, HTTP, secure-copy, native-backing
self-tests pass. Candidate D:/Tools/psemu-build/query-output has matching map.
Actual runtime timing and first recognizable Astro frame remain pending.

## 2026-10-27: track private backing allocation coverage

Page-query candidatePID5048 first capture~115s (18:30:59) vs search-only~375s;
first intervals18/17/15/14s, later~13s. All first8 captures remain black/dither.
Actual UI-transition asset opens occur~38s vs prior~228s. This is measured
loading/frame-time progress, not a recognizable first game image. ByT471 the
game continues opening real effects resources throughfd2383, without a fatal.

Three complete thread samples identify remaining graphics thread15288 inside
NtQueryVirtualMemory -> SharedDirectMemory::Readable(+0x74) -> TryRead ->
LibKernel::Memory::TryReadBacking -> BufferCache::ObtainBuffer. Private backing
permissions are changed ONLY by SharedDirectMemory (no exported alias accessor,
no decommit API); guest GPU guards live independently on the guest view.
Record successful page-aligned Commit coverage under the existing commit mutex,
merging adjacent/overlapping intervals. TryRead/TryWrite validate the full range
against that owned allocation map without querying mutable guest permissions or
walking gigabytes of private backing mapping. Allocation holes are still
rejected before any bytes copy; failed/out-of-bounds commits do not add coverage.
Commit's original backing-query/new-page/guest-guard logic remains unchanged.
This uses actual owned allocation state, not a stale cache of guest page flags.

Both backing regression modes pass: pagefile and16GiB D-sparse file,10GiB logical
allocation within commit headroom, repeated/extended commit preserving guest
NOACCESS/READONLY and bytes, exact bounds/overflow, disjoint allocation holes,
unaligned page commitment, adjacent/overlapping coverage merging, and1000 real
reads concurrent with a separate commit. D scratch deletion also verified.
Full local build and secure-copy/native-backing self-tests pass. Candidate
D:/Tools/psemu-build/backing-query-output includes matching map. Runtime frame
speed/first visible image pending; old query-only run stopped intentionally for
this candidate, not classified as a crash.

## 2026-10-27: reach ps_logo; implement captured R32Float RRR1 depth mip store

Owned df6e8fd runPID2384 first capture~99s, steady~0.8-1.2s/frame (0.8-1.3FPS)
vs prior~13s/frame and older~60s/frame. Still black, not a recognizable frame.
ui_pause_hub loadedT358 (~2295 in older run), real resource opens passfd6449.
StartLevel ps_logoT518 and Level has started: ps_logoT541 are actual guest logs.
Guest also emits pool-membership warnings/int41; their cause remains unresolved.
They are not the terminal cause observed here. AtT561 the renderer exits321:
R32Float storage image480x270, depth tile24, swizzle0x324(RRR1), base/last0,
MaxMip7, backing0x160000, write-only. Exact descriptor:
5a625f00 c1600001 00434077 91800324 00000000 00700070 00000000 00000000.
Log prefix tools/data/astro_20261027_backing_query, captures run_20261027_183920.

Support RRR1 for already-supported single-channel storage formats and identity
Vulkan storage views; existing production inverse-store mapping selects sourceX
for physical R. Nonidentity storage reads remain rejected. Permit write-only
R32Float depth mip0 views backed by a validated full mip chain. New constexpr
PlanDepthStorageMipLayout validates 2D/base0/view1, format/depth, dimensions,
64KiB alignment, exact pitch, mip count and checked exact total size. Each mip
uses its actual128x128 depth allocation/pixel permutation, not render-target
mip tails. Captured eight offsets are0,c0000,100000,110000,120000,130000,140000,
150000; total160000. Existing depth upload already detiles each native mip.
Tiler and image-to-buffer reconstruction now retile every mip and preserve
all backing padding; all native mip data is downloaded, so unmodified mip
pixels remain intact. No descriptor bypass, dummy output or game-byte patch.

Production-extracted depth tests verify the real captured8-mip layout, invalid
shape/overflow-style constraints without altering output, bit-exact roundtrip
of every mip, padding/canaries and unchanged trailing mips after a mip0 edit.
New offscreen Vulkan test uses actual production InverseStorageSwizzleComponent
selector, an identity R32Float mip0 view of an8-level image, GLSL compute write,
and real upload/download. Arc B580 passes129600 mip0 pixels with7 unchanged
mips and all host padding preserved. This device test is NOT the actual Astro
shader and not a recognized game frame. glslang + spirv-val pass. Full local
build succeeds at D:/Tools/psemu-build/depth-mips-output with its matching map.
Actual Astro retry remains pending.

## 2026-10-27: real binary32 classification for telemetry imports

Read-only dump analysis identifies trapRVA10a224e in Telemetry.cpp's float
NaN/INF validation (line453 in code), not proof of the separate pool-membership
warning. CallRVA74ecd90 is PLT1472 __isfinitef; runtime logs show its default0
return even for finite data. Existing core implemented only double fp_isfinite.
Add true binary32 __isfinitef/__isnanf/__isinff in native SysV dispatch and real
VEH fallback. Bit classification preserves sNaNs and MXCSR instead of performing
FP arithmetic; signed zero and subnormals are finite, infinities/NaNs are not.
Tests use actual production SysV functions and extracted VEH branch, known
binary32 categories, poisoned upper XMM bits and unchanged MXCSR. Passed.
No forced-finite result, bypassed game assertion or altered fp_isfinite(double).
Local Jetsku reference search found no __isfinitef implementation to reuse.

## 2026-10-27: preserve GPU storage owner across transient partial overlap

c766b65 retryPID9492 accepts the previous R32Float RRR1/mip descriptor, but
later exits321 at19:08:44 (~515s), with ps_logo startedT511. New exact error:
requested184edd30000+160000 versus existing184ede50000+c0000, kind1(StorageTexture),
same context, native/trackerGPU-dirty, CPU/buffer clean. Existing old single-mip
owner straddles the new8-mip backing's right edge (offset120000; old end1e0000,
new end160000). Never discard this GPU data or change descriptor addresses.

Add a strict storage->storage reconstruction predicate for validated R32 depth
mip backings, actual byte overlap, consistent native/tracker GPU ownership,
same context, no CPU/yielded/buffer owner, and no cached-buffer page overlap.
Existing exact-match reuse runs earlier and remains unchanged. Retirement
isolation still rejects any retained page alias. Retain the old StorageTexture
on the recording command until fence, as already done for render targets.
Use the existing real readback/publication transaction before retirement.
ReadbackWorker::DownloadColorImage now generates one correct region per depth
mip, downloads all of them and retiles with original padding preserved. Other
unsupported multi-mip storage layouts remain rejected. This also completes the
worker path separately from the buffer-transition path fixed in c766b65.

Production tests cover the captured partial overlap both directions, disjoint
ranges, invalid old layout, and every rejected ownership/context state. Actual
depth conversion publishes the complete old range including its out-of-request
tail, with external canaries and pixel roundtrip checks. Depth and float tests
pass. Full local build and secure-copy/clock/native-backing self-tests pass;
previous Arc B5808-mip upload/compute-write/download test remains passing evidence
for the mip data path, not proof of this full game alias transition. Candidate
D:/Tools/psemu-build/storage-alias-output includes matching map. First recognizable
Astro frame and actual alias/telemetry retry remain pending.

## 2026-10-27: share storage backing across independent sampled mip views

f6b4138 runtimePID15468 confirms STORAGE-STORAGE-RETIRE and complete publication
of old247e3cf0000+c0000 before requested247e3bd0000+160000; previous partial-alias
fatal is passed. Telemetry int41 traps are absent after native float classification.
ps_logo startsT616; next fatal exit321 at19:29:12, owned prefix
astro_20261027_storage_alias. Exact new failure is sampled/storage alias on the
SAME R32Float backing480x270, pitch512, depth tile24,8 mips,swizzle324: sampled
view_levels8 versus cached storage view_levels1, both base0. It is a view mismatch,
not changed backing or permission to discard GPU pixels. All captures remain black.

ClassifyStorageSampledOverlap now compares physical backing independently of
base/view mip selection. Both views must separately be nonempty and wholly
inside1..16 backing mips. Format compatibility, CPU-clean/GPU-current ownership,
same context, physical shape/address/size/pitch/type/base-array checks remain.
Existing GetStorageTextureSampledView creates the real in-bounds Vulkan sampled
view on the retained image; no CPU reupload, fake result or descriptor replacement.
Tests cover the captured8-mip view over1-mip storage view, valid last-mip subset,
empty/overflowing views, physical backing mismatches and ownership/context errors.

Extended offscreen Arc B580 test binds the SAME R32Float8-mip image as storage
mip0 and a real sampled RRR1 view of all8 levels. Compute checks textureQueryLevels
and texelFetch of every unchanged mip1..7 including RGB replication/alpha1, then
writes129600 mip0 pixels using actual production inverse-swizzle selection.
All sampled checks, unchanged lower mips and host padding pass; glslang/spirv-val
and production depth tests pass. Full local build sampled-mips-output completed
19:35:32 with matching loader.map; review quota paused subsequent commands until
user resumed22:40. Targeted tests and native-backing/clock self-tests pass again.
First recognizable Astro frame and actual game sampled-view retry remain pending.

## 2026-10-27: bind depth storage views to every valid backing mip

eb97f1b retryPID18080 gets beyond the earlier sampled8/storage1 view mismatch.
Next real fatal exits321 at22:51:43: same R32Float480x270 depth mip chain,
size160000, MaxMip7, RRR1, but output now selects base_level1/last_level1.
Exact descriptor8c689600 c1600002 00434077 91811324 00000000 00700070 0 0.
IsSupportedStorageTextureDescriptor still required depth-float base0/last0;
PlanDepthStorageMipLayout also conflated base view with physical mip layout.

Allow any nonempty in-range single storage mip for the existing full R32 depth
backing. Native descriptor predicate retains its base==last, last<=MaxMip,
write-only/depth/format/type/swizzle/encoding checks. Physical mip plan validates
base<levels while keeping the full unchanged allocation offsets. Native image
creation already allocates the complete chain from physical mip0 and the existing
GetStorageTextureStorageView supplies the selected single-mip identity Vulkan
view; EqualStorageBacking already ignores view selection. No address/LOD spoofing.

Actual production descriptor and encoding validators are now extracted in the
CPU test: captured descriptor passes mip0..7 and rejects reversed/multimip/out-
of-range storage views and nonidentity reads. Physical plans for all selected
mips have identical offsets; existing invalid bounds/padding/ownership tests pass.
Offscreen Vulkan test accepts target mip and samples all OTHER mips through the
same full-chain RRR1 view, verifying query-level count/channel values before
writing. Arc B580 passes targets0(129600 pixels),1(32400),7(6), with7 untouched
mips and preserved host padding each time. GLSL/SPIR-V validation passes.
Full local build storage-levels-output completed with matching map. Actual Astro
retry and first recognizable game frame remain pending.

Opening-sequence clarification from actual old logs: sceAvPlayerInit returns0,
then Stop/Close run; game proceeds from ps_logo to title_controller_ship resource
loading. ps_logo log is not a rendered-logo claim. Target remains a genuine
Vulkan-rendered title/game frame, not a fabricated or externally decoded image.

## 2026-10-27: publish overlapping GPU storage before sampled reconstruction

7d43a47 retryPID20700 reached title_controller_ship loading and exited321 at
23:06:41.503. The next sampled/storage alias was a 480x270 R32Float depth view,
address backing+0x120000, size0xc0000, levels1, RRRR (0x924), overlapping an
8-mip storage backing of size0x160000, RRR1 (0x324). This is a different physical
allocation, so sharing a Vulkan mip view would be incorrect.

CanReconstructStorageForSampled validates the sampled view separately from the
physical depth allocation and retains strict GPU/current/CPU-clean/same-context/
no-buffer ownership checks. FindTexture now publishes the full old GPU storage
allocation through existing readback/retile/retirement before preparing a sampled
source. Bytes outside that allocation retain their prior contents. Command
recording generation changes abort descriptor binding and retry, matching the
existing storage path; no stale image pointer is dereferenced after submission.

CPU tests pass for ownership/view rejection and the captured partial overlap:
production depth conversion preserves all out-of-allocation bytes and decodes
the shared sampled tile to the known mip4 pixels. Local full build completed
23:14:15 in sampled-reconstruct-output with matching loader.map. Real Astro
retryPID9508 started23:17:57. First recognizable game frame remains pending.


## 2026-10-27: exact sub-page image sources with complete page coherence

b73331e retryPID9508 passes the real previous storage/sampled alias: atT534.37
STORAGE-SAMPLED-RECONSTRUCT publishes the eight-mip owner, then game loading and
shader compilation continue (46 -> 48 shaders). Next fatal exits321 at23:26:54:
ObtainBufferForImage rejected address0x1f51b12a800 size0x1000, page_aligned=0.
This is a sub-page texture source, not an invalid or out-of-address-space range.

Keep the exact requested image address/size/native-buffer offset. Expand ONLY
coherence operations to enclosing tracker pages: collect and download every
exact dirty byte range on those pages before clearing tracker/byte ownership,
then upload CPU-dirty cached page fragments. Edge-page bytes outside the image
must not be dropped or overwritten with stale guest data. Address-space and
empty/overflow guards remain. Existing page-aligned requests keep their range.

New test_buffer_image_source.ps1 compiles actual production ObtainBufferForImage
and RefreshCpuImageFragmentsLocked with the production RangeSet. Simulated
tracker/transfer/backing dependencies check both edge-page GPU writes outside
the texture, exact GPU byte copies/canaries, next-page ownership retention,
one-byte views, CPU page refresh, exact source offset/extent, cache-hole fallback
and invalid/boundary addresses. PASS. This is a CPU harness, not an Astro frame.
Local subpage-image-output build completed; real retryPID7300 starts23:29:37.
First eight previous captures remained black/dither; recognizable image pending.

Follow-up validation: aligned existing image requests perform no redundant
transfers; two cached fragments separated by a hole refresh both complete edge
pages, preserve CPU ownership in the hole, and return the exact guest fallback
extent. PASS. Added test_buffer_image_source.ps1 to Windows CI. Local Jetsku
reference src/graphics/host_gpu/renderer/cache/bufferCache.cpp likewise accepts
any valid GuestRange in ObtainBufferForImage and returns the exact buffer offset;
it does not impose a tracker-page-aligned texture address.

## 2026-10-27: straddling render target to storage reconstruction

126cc13 retryPID7300 passes the sampled/storage handoff and sub-page source
(previous BufferCache fatal absent), compiling49 shaders. Next real fatal
exits321 at23:38:45: storage0x183de6c0000+0x1a20000,2432x1368 RGBA16Float,
render-target tile27, intersects old target0x183dfb30000+0x870000,1920x1080,
4bytes/pixel. Same context, GPU/tracker current, no CPU/yielded ownership.
Old target begins at new+0x1470000 and extends0x2c0000 beyond the new allocation.
Both footprints are already supported, but CanReconstructRenderTargetForStorage
required one allocation to contain the other.

Allow actual byte overlap once all existing ownership/layout/page/bounds checks
pass. Existing ResolveStorageImageOverlaps still validates retirement isolation,
retains old images until their fences, downloads/publishes the FULL old target,
and refreshes clean cached buffer fragments before replacing the allocation.
No tail truncation and no simultaneous stale owner is permitted.

Production CPU regression passes the exact captured shape: full old RGBA8 retile,
independently expected per-pixel permutation, nonempty pixel tail outside the new
storage allocation, unchanged padding/prefix/canaries, and full-range publication
across a cached buffer tail. Ownership/conflict/malformed/disjoint checks remain
negative; formerly rejected valid partial overlaps now pass. Local build pending.

## 2026-10-27: packed R11G11B10 RGB1 DCC storage

157dc0e retryPID21980 passes the straddling target guard, compiles50 shaders,
then exits321 at23:50:51 on new R11G11B10Float(format36) output. Captured
1920x1080+0x870000 RGB1(0x3ac), write-only2D, descriptor:
5b632f00 c2400002 010dc1df 91b003ac 00000000 00700000 207b0000 025b96c3.
Existing DCC-storage encoding accepted only RGBA16F and native storage views
rejected the RGB1 swizzle for packed float.

Accept this specific format/swizzle in descriptor/view validation; native storage
view stays identity and the existing production inverse swizzle routes RGB to
physical RGB, with absent alpha discarded by the packed format. Retain rejection
of nonidentity storage reads. DCC storage now computes metadata from actual
4/8 byte format: packed1920x1080 uses512x512 metadata blocks,49152 bytes, while
RGBA16F keeps512x256 and81920 bytes. Only the two validated formats/swizzles and
existing exact captured metadata-control bits are accepted; full-slice inspection,
clear/expanded-only decoding, synchronization and alias/conflict checks remain.
Render-target reconstruction validates packed storage's4-byte physical backing.

Captured production descriptor/encoding/swizzle positives and malformed/reserved/
read-access negatives PASS. Production DCC footprint/binding regression PASS.
Arc B580 Vulkan test writes2073600 pixels via formatless OpImageWrite to native
B10G11R11UfloatPack32, barriers, samples same image through RGB1, writes again,
then checks exact packed(.25,.5,1) bits and untouched row padding on host readback.
Production inverse swizzle is extracted for this test. GLSL/spirv-val PASS; existing
R32 depth targets0/1/7 also PASS. Emulator device already enables the required
shaderStorageImageWriteWithoutFormat feature. This fixture is not Astro execution.
Initial packed-dcc-output build passes; full packed ownership build/test pending.

Full packed-dcc-complete-output build finished successfully; matching loader.map
copied. Intermediate PID19112 was intentionally stopped after verifying its exact
executable path while still booting (not a game failure). Complete retryPID20136
started2026-10-28T00:00:19, log prefixastro_20261028_packed_dcc_complete.
The packed Vulkan fixture additionally initializes native image with transfer
clear(1,1,1,0), samples through RGB1 to verify(1,1,1,1), performs formatless writes,
barriers and samples again before checking all2073600 packed pixels and padding.
PASS on Arc B580. R32mip0/1/7 regressions PASS. No production change for this added
clear check. Actual complete Astro run is still loading; first image pending.

## 2026-10-28: sampled use of a readback-clean storage image

Complete ea5df10 retryPID20136 passes packed RGB1 DCC descriptor/storage creation
and compiles51 shaders. Next fatal exits321 at00:09:43: exact same R11G11B10
1920x1080 backing+0x870000, swizzle3ac, native storage gpu_modified=0,
cached cpu_dirty=0, same_context=1. Existing ClassifyStorageSampledOverlap
required gpu_modified=1 even after a readback leaves BOTH copies current.

Add a distinct CleanImage classification for exact compatible physical backing
with valid sampled/storage views and clean cached CPU state. Before reuse,
IsCoherentSampledStorageOwner requires no yielded buffer ownership, no cached
or tracker CPU dirtiness, and identical cached/tracker GPU state (both dirty or
both clean). Native view validity and fence retention remain. Sampling the clean
copy does not falsely mark it as a GPU write. Inconsistent ownership remains fatal
with explicit buffer/cached/tracker diagnostics. Bounded STORAGE-SAMPLED-CLEAN
logs identify actual reuse. Production tests cover captured packed clean state,
R32 full-chain clean views, GPU-current views, stale buffers, CPU dirtiness and
tracker mismatch. PASS. Full local clean-storage-sampled-output build pending.

## 2026-10-28: depth storage backing reused as packed-color storage

1a73b96 retryPID16584 successfully logs STORAGE-SAMPLED-CLEAN twice atT506.8
for the previously rejected R11G11B10 source. Next fatal exits321 at00:22:20:
requested960x540 R11G11B10, pitch1024, tile27, same address/size0x280000 as an
existing storage owner; GPU/tracker current, CPU clean, same context, no buffers.
Earlier TEX#10 identifies that address as R32Float tile24. The fatal omitted old
storage info; diagnostics now include its full format/shape/mip/tile/swizzle.

Extend the existing strict depth-storage readback admission to a validated single
mip packed-color/RGBA16F destination. Old owner must STILL satisfy the R32 depth
layout validator; no arbitrary storage old-owner acceptance. Shared color-backing
validator reuses the existing render-target physical-footprint logic and preserves
bounds/type/mip/alignment and competing CPU/GPU/buffer rejection. Existing full
readback retains old image through its fence, publishes every guest-layout byte,
retires it, then uploads a native image of the requested format.

Captured960x540/1024/0x280000 CPU test passes: production depth retile preserves
padding/canaries, production render-target detile reads the published bits,
full per-pixel comparison passes, malformed bounds/pitch/ownership reject.
The 32-bit depth and render-target permutations match for this footprint; an
initial test expectation of DIFFERENT permutations was wrong and corrected to
equality. The format transition must preserve bits rather than convert float
numeric values. Regression failures now print source location. Local build pending;
first recognizable image remains pending.

## 2026-10-28: preserve color storage padding on reverse depth reuse

52319a9 retryPID21996 passes depth -> packed color, compiles53 shaders, and then
exits321 at00:37:42 on the reverse transition. Full diagnostic confirms exact
same960x540,pitch1024,0x280000 allocation: old format36/tile27/RGB1 storage,
new format22/tile24 R32 depth, GPU/tracker current, CPU clean, no buffers.

Allow either validated color or depth footprint as the old storage owner, keeping
all existing context/ownership/layout/bounds/isolation conditions. DownloadColorImage
now routes validated single-mip color storage through the existing padding-
preserving RT32/RT64 reconstruction, using original guest backing for padding.
Other storage layouts keep their old handling; no unknown compressed formats or
buffer conflicts are accepted. Full old allocation/fence lifetime publication
and retirement remain unchanged before the new depth image upload.

Production CPU test covers both directions and negative ownership/bounds. After
packed-color retile, preserved canaries/padding match prior backing and every
R32 depth pixel contains the exact packed bits. PASS. Existing CPU regression
suite passes. Local storage-layout-roundtrip-output build pending; recognizable
Astro image still pending.

## 2026-10-28: full storage publication across clean cached buffer fragments

Resumed3d3533b runtimePID20016 started11:42:49 and exited321 at11:53:40.
Next alias: requestedR32 depth480x270,pitch512,size0xc0000 atold+0x240000,
oldRGBA16F2432x1368,pitch2432,size0x1a20000. Old GPU/tracker current, CPU clean,
no yielded owner. Old fatal printed buffer overlap for the SMALL request only;
storage admission used HasPageOverlap over the FULL old allocation. Therefore
clean resident fragments outside the request could reject a legal publication.
Old-buffer dirtiness/type were omitted, so diagnostics now expose them explicitly.

Storage reconstruction now follows the existing RT rule: reject buffer GPU-dirty
or cached CPU-dirty owners across the full old range, rather than mere residency.
Explicit full storage readbacks use the preserve-backing mode: supported depth/
color layouts may traverse clean buffer fragments, ReconstructImageBacking
preflights all competing owners BEFORE download/retile/backing replacement, and
marks the full range CPU-dirty for later fragment upload. Fault-driven storage
readback restrictions and pending-fault-page handling remain unchanged. Cached
image CPU-dirty state is checked too. Storage aliases drain pending commands via
the existing pre-lock recording-generation retry path, as RT aliases already do.

New production CPU test covers captured largeRGBA16F -> smallR32 shape with a
cached tail outside the request. Full retile matches independently expected
visible pixels, padding/canaries and full-range publication. CPU-dirty tail,
GPU-dirty tail and byte/tracker disagreement each reject before callback,
leave all bytes unchanged, and publish nothing. PASS. Full local build completed
in storage-clean-fragments-output with matching map; actual retry pending.

## 2026-10-28: Astro compute SOPC0x12 / S_CMP_EQ_U64

2cd7478 retryPID21096 starts12:02:47 and exits321 at12:11:22. Storage aliases
advance to a new compute decoder failure atPC0x3bc, wordbf128002, SOPC0x12.
The full1536-byte CS was already saved locally as
_Shaders/original/0384_0053_new_shader_cs_failed_00000213a76cb500.bin
No game shader binary is committed.

Pinned local Jetsku reference frontend/decode/ScalarAluOps.cpp maps0x12 to
S_CMP_EQ_U64, and frontend/translate/Scalar.cpp emits IEqual64 into SCC.
Add decoder/name/IR mapping, full source-pair register collection, compare-op
classification and dispatch. Native SPIR-V compares low/high32 independently
with IEqual and combines them with LogicalAnd, then uses the existing SCC result
writer. No shaderInt64 feature is required and no branch/operand is forced.

Production semantic fixture now checks equality/inequality/unsigned ordering
on edge matrices,10000 random pairs, same pairs and upper-only differences;
existing vector borrow tests remain passing. Dedicated full-source pipeline test
checks the captured raw operand/PC, upperSGPR3/7 collection, scalar SCC lowering,
actual resource planning and Compute SPIR-V emission for equality and inequality.
Both modules pass Vulkan1.2 spirv-val. CI default invocation PASS. It compiles
production sources independently; attempted temp-object reuse was removed because
the existing clang driver does not retain those intermediate objects.

Offline replay of the actual failed CS decodes207 instructions into10 CFG blocks,
lowers all instructions and recognizes5 EqU64 instructions. Captured replay does
not materialize its real descriptors or execute the game GPU pipeline; this is
not an Astro frame. Full local scalar-eq-u64-output build passes with matching map.
Real retryPID18124 started12:25:00, log prefixastro_20261028_scalar_eq_u64.
Reference commit: f9e1958
## 2026-10-28: scalar pair user-data dependencies and next mip failure

d5df708 retry PID18124 exits321 at12:33:54 after compiling60 shaders.
The former SOPC0x12 failure is passed in the actual game. The next stop is
NativeTexture mip validation: base6,last6,physical levels6. This is not a
recognizable Astro frame. The diagnostic candidate logs address, dimensions,
texture type/tile/resource kind/read-write flags and all eight descriptor dwords;
PID14884 starts12:39:08 with prefixastro_20261028_mip_descriptor.

Review of the new U64 comparisons found a distinct upper-SGPR binding omission:
CollectRegisters allocated both words, but AttachScalarSources and CollectUserData
only retained the low-word provenance. Preserve high-word provenance separately
for EqU64/NeU64/GtU64 and collect it at binding allocation. Comparisons continue
to write only SCC. Do not classify their result as a 64-bit destination.

Regression extends the production pipeline with standalone SGPR2:3 versus6:7
comparisons and an overwrite ofSGPR3 before the compare. Expected bindings are
{2,3,6,7} and{2,6,7} respectively. Generate and validate the actual user-data
SPIR-V modules as well. Local build scalar-upper-binding-output passes with
matching map. Six generated modules pass Vulkan1.2 spirv-val; production pipeline regression and
captured CS decode/CFG/IR replay PASS. Real-game validation pending.

Mip diagnostic PID14884 reproduces the failure after60 shaders. Descriptor:
497f8500 c4700001 010dc1df 91b66fac 00000000 00700050 00000000 00000000.
It is a written-only RGBA16F1920x1080 2D tile27 view of mip6 while MaxMip=5.
Keep this captured failure for subsequent image work. User now asks to investigate
actual audio output to provide independent evidence of game progress.

## 2026-10-28: actual AudioOut2 PCM output path

User requests an audio investigation as independent evidence of Astro progress.
Existing core only paced AudioOut2 queues. GPU libs/libAudio2.cpp and audio.cpp
contain PCM support but are excluded from GPU_LIBS_SRC and depend on SDL.
Add a loader-native Windows waveOut path without changing graphics checks.

GuestAudioOutputCall now creates distinct64-bit users/ports, returns actual port
channel counts, stereo speaker info, and retains PCM attribute0 pointers per port.
ContextPush copies validated guest PCM at successful queue admission. Support
48kHz interleaved float/s16 mono/stereo/8-channel beds with normalized stereo
mixing, finite-value handling and bounded eight-buffer asynchronous host queues.
Keep host buffers until WHDR_DONE and reset/unprepare/close on context destruction.
Do not play object/haptic ports as speaker beds; measure object PCM separately.
No new DLL/runtime dependency; winmm was already linked.

Astro actually creates main/BGM float8ch format0x800 ports at48kHz,512frames,
plus19 mono object ports(type0x100,format0x100). Correct sceAcmContextCreate
separately to publish its32-bit context ID; pinned Jetsku acm.h/cpp confirms ABI.
The reference ACM batch functions themselves are placeholders that advance command
positions and return success; they do not supply a complete DSP implementation.
No ACM batch processing or spatial renderer is claimed here.

Production source regression uses mocked waveOut calls, so it plays no synthetic
hardware audio. Covers ABI sizes, invalid pointers and guards,32-bit ACM output,
user/port handles,7.1/mono/s16 mixing,NaN/Inf handling, guest-buffer copy ownership,
full host queue/drop/reuse and reset/close, and object isolation. PASS. Separate
LFE-only fixture proves nonzero source PCM can produce a zero stereo mix; raw peak
and cumulative input_nonzero diagnostics distinguish this from actual silence.
CI now runs test_guest_audio_output.ps1; existing queue regression remains PASS.
Full local build audio-final-output PASS, with matching map, all files onD.

Real PID11232 first discovers8ch output (stopped to add that format). PID22580
then observes thousands of zero stereo submissions (stopped for ACM/object probe).
PID23608 starts12:57:32, prefixastro_20261028_audio_acm. Main mix stayszero over
13312pushes while19 object ports becomezero after a short initial out-of-range
transient. No valid audible Astro output has been obtained. This running candidate
precedes raw input_peak diagnostics; do not interpret a zero downmix as proof all
source channels arezero. Final raw-metric candidate is built but not run yet.

Audio ACM candidate PID23608 exits321 at13:07:16 with the same Vulkan mip6/max5
error after60 shaders. Last periodic audio record:28672pushes,3 active bed ports,
19 object ports,zero mixed peak andzero nonzero submissions,zero invalid pointers.
Thus the audio thread is running; no audible game output was obtained before the
independent graphics failure. This does not prove every raw bed channel iszero.
Final candidate PID21860 starts13:08:56, prefixastro_20261028_audio_final, and now
logs raw input_peak/input_nonzero in addition to the actual stereo mixture.

First eight final-candidate pushes confirm raw input_peak=0 and cumulative
input_nonzero=0 for active bed PCM. Initial object transient is measured separately
and is never routed to speakers. Actual waveOut device opening remains untested
because no nonzero bed submission has been observed; mock backend regression PASS.

## 2026-10-28: raw PCM remains zero; unblock the actual graphics stop

Raw-metric PID21860 exits321 at13:17:22 at the same RGBA16F mip6/max5 descriptor
failure. Last periodic record26624pushes:3 bed ports,19 object ports, input_peak0,
input_nonzero0,mixed peak0,nonzero0,invalid pointers0. No audible Astro PCM found.
Read-only import-map probe confirms ACM Create1, BatchStartBuffers0, BatchWait0;
these are actual call counters, not merely a log-filter observation. Imported map
has1708 entries. Do not infer that the software mixer is stopped from zero ACM
batches: e03cf0 is called by the output loop, pending_ticks is1; in the next run
12 render-list nodes have their enabled byte+0x120 set. No guest state was changed.
Node fields+0xb8/+0x110 are opaque class/routing tags, not confirmed channel counts.
Likewise engine+0x740 is not a verified ACM context field; discard that probe label.

To reach later sound events, fix the independent graphics abort rather than bypass
rendering or force nonzero sound. ShaderTextureResource::NonMsaaMipLevels includes
both MAX_MIP and LAST_LEVEL, as pinned Jetsku BuildTextureDescription does for its
physical/view level bounds. NativeTexture and storage validation share natural
mip-range checks. Captured1920x1080 RGBA16F view6 with MaxMip5 needs7 host levels.
Keep BaseLevel6; do not rebase to0 or clamp to5. Reject inverted ranges and mip
counts exceeding the resource dimensions (3D depth contributes; array layers do not).

Production descriptor regression PASS, including mip0..10 and invalid11..15.
R32 natural mip8 now valid even with MaxMip7; replace old invalid8 fixture with9.
New production tile27 extraction plus actual gpu_format.cpp shows levels6 and7
share backing0x1640000, all original0..5 offsets/padded layouts stay unchanged,
new mip6 is bounded within the same64KiB tail and disjoint from mip5. PASS.
CI now includes test_texture_mip_view.ps1. Existing depth/cache/ownership tests PASS.

Native Vulkan fixture extended with RGBA16F1920x1080,7 mip levels, view at mip6.
ArcB580 formatless imageStore writes480 pixels and reads all four half-float
channels back bit-exactly; sampled image reports7 levels, other6 mips and padding
stay unchanged. Existing R32 mip0/1/7 and packedRGB GPU cases also PASS. This is a
standalone fixture, explicitly not an Astro frame or evidence of audible audio.

Full audio-mip-view-output build PASS with matching map. PID21952 starts13:21:35,
log prefixastro_20261028_audio_mip_view; real progress beyond the former abort
remains to be validated. Root Sarah executable and user launch scripts untouched.

## 2026-10-28: validate Windows output and canonicalize the existing RGBA16F tail

The first-bed candidate opens waveOut for the first valid game PCM bed buffer,
including real zero PCM. It no longer hides device initialization until a nonzero
sample. Mock regression verifies initial silence is submitted without incrementing
nonzero, then owned nonzero buffers, bounded queue reuse/drop and teardown. PASS.
Actual PID23072 audio-device-output logs waveOutOpen=0 at48kHz stereo and0 host
drops. Game input remainszero in the observed samples; this is not audible audio.

Mip-view candidate PID21952 passed NativeTexture's former mip6/max5 validation
but then failed the next genuine ownership check: same address and0x1640000 bytes,
RGBA16F1920x1080 pitch1920 tile27, old storage6 levels,GPU/tracker current, CPU and
buffer owners absent; requested storage7 levels. No earlier mip data may be lost.

Instead of discarding that image or uploading stale old mip bytes, canonicalize
this exact kind of physical allocation when first created. Production tile API
TileGetEquivalentRenderTargetMipLevels permits only2D RGBA16F render-target tiles,
checks the actual allocation byte size and natural mip bound, and requires every
existing mip's full source/linear offsets, sizes, tail coordinates and padded
extent to remain identical. Both sampled/storage cache entrances use the resulting
physical capacity. Guest BASE_LEVEL and visible view_levels remain unchanged.
The captured6- and7-level descriptors converge to11 physical levels in the same
0x1640000 bytes. Initial coherent upload includes the tail before GPU ownership;
no in-flight GPU image is overwritten, and tracker ownership rules are unchanged.
The pinned Jetsku SameBacking likewise separates backing capacity from view bounds;
our canonicalization uses the local production tile layout as its admission proof.

Production CPU fixture checks capacities6/7/11 ->11, single-mip/other format/wrong
pitch/wrong size/naturally invalid inputs unchanged, exact ImageInfo preservation
apart from levels, storage mip6 view1, sampled view7, and scope exclusions for
arrays/volume/base-array/invalid views/tile/size. PASS. Existing depth/storage and
AudioOut2 regressions PASS. Full local audio-mip-tail-output build PASS.

ArcB580 Vulkan fixture covers both7-level backing/view7 and11-level backing/view6,
with storage view at mip6. All480 written texels match four half-float channels,
other6 or10 mips and linear-copy padding stay unchanged, sampled query reports
only the visible view count. Existing R32 mip0/1/7 and packedRGB cases PASS. These
are standalone GPU tests, not a native Astro frame or an audible game recording.

PID19132 starts17:39:43 in audio-mip-tail-output, prefixastro_20261028_audio_mip_tail.
Actual passage of the former cache alias stop is pending. Tools, logs, builds and
all new artifacts stay onD. Prior canonicalization write had been rejected by the
automatic approval review's usage quota and did not execute; it is now applied.

## 2026-10-28: preserve narrow sampled views of canonical storage backing

PID19132 exits321 at17:49:35. The cache creates11 levels, then the binding path
rejects a legitimate sampled view (base0, requested6 levels, view6) because
GetStorageTextureSampledView still required image_levels==info.levels. This check
should have been included in the canonical-backing change; capacity and visible
range are separate. No evidence of audible sound or a visible Astro frame.

IsValidStorageSampledMipRange now requires nonzero bounded physical/requested
counts, requested<=physical, base<requested and nonempty view<=requested-base.
Use subtraction for overflow safety. GetStorageTextureSampledView retains the
exact requested base/count/swizzle and all type/format/layer validation. It never
substitutes the11-level capacity into the visible6-level view.

Production regression covers6-view/11-backing, exact6, base6/count1 in requested7,
zero/too-small/too-large backing, zero requested count, requested count exceeding
backing, base at/past range, empty/overlong and UINT32_MAX base/count. PASS along
with existing depth/storage ownership tests. Earlier actual ArcB58011-backing /
6-view GPU fixture already proves that exact native view shape and bit readback.
Full local audio-sampled-bounds-output build PASS with matching map. New real
PID20656 starts17:53:58, prefixastro_20261028_audio_sampled_bounds.

## 2026-10-28: partial formatted-buffer alias of linear RGBA16 UNorm storage

Sampled-bounds PID20656 passes the prior view/canonical mip stops and compiles60
shaders. Next fatal: GPU invalidation of write0x2246a640000+0x480000 overlaps storage
0x2246a940000+0x3f4800, GPU current,buffer markerfalse,formattedtrue. Captured TEX#56
confirms960x540,fmt65(k16_16_16_16UNorm),tile0,identityRGBA. Its0x3f4800 bytes end
halfway through a4KiB page. The old generic classifier rejects non-page-sized
storage and crossing writes. Do not broaden that generic classifier.

Add a narrow admission/readback path for validated single-level2D linearRGBA16
UNorm with exact256-byte row pitch/allocation size, identityRGBA and no array or
volume. Allow crossing formatted page-aligned writes only after proving current
image/tracker GPU ownership, no CPU/competing-buffer owner, no other image page
owner and no metadata. Buffer-owned stale images may be invalidated without
reading their old native image; contradictory ownership is rejected.

The dedicated readback waits for prior GPU work, uses BufferCache's existing
ReconstructImageBacking preflight callback, snapshots the full guest allocation,
downloads the native image and copies only visible rows onto that snapshot. This
preserves row padding and bytes outside the impending write; writes only the
actual image byte range, then publishes the full image range. Edge-page isolation
is mandatory before clearing its tracker pages. No outside-image bytes are erased.

Production helper regression covers the exact0x300000-offset crossing write,
opposite edge crossing, buffer-owned reuse, unformatted/unaligned/nonoverlap/
overflow ranges, mismatched owner/tracker/CPU/page/other-buffer state,12 unsupported
layout changes, all captured pixel bytes,959-of960 row padding and guard bytes.
Actual ReconstructImageBacking callback tests verify full-range publication and
GPU tracker/range/CPU-fragment conflicts rejecting before callback or mutation.
Existing depth/storage ownership tests remain PASS. Full local build PASS.

Actual ArcB580 fixture adds960x540RGBA16UNorm storage:518400pixels, four exact
16-bit UNorm channel values, sampled view and padding checks PASS; previous R32,
packedRGB and RGBA16F7/11-level cases remain PASS. Standalone native API evidence,
not an Astro frame or audible recording. PID24396 starts18:16:23 in
 audio-linear-storage-output, prefixastro_20261028_audio_linear_storage.

AvPlayer audit additionally confirms the local FFmpeg-backed implementation is
excluded from GPU_LIBS_SRC. Current core returns0 for Init and game takes Stop/
Close without source/data calls. Treat video/audio decoder integration as separate
unfinished work; do not count externally playing a movie as emulator progress.


### 2026-10-28: disjoint image owners in a formatted buffer write

The linear-storage run reached a write spanning the end of a native render target and the beginning of the adjacent linear UNorm storage image. Captured RT `0x16eeb170000+0xff0000`, storage `0x16eec160000+0x3f4800`, write `0x16eebe60000+0x480000`. The owners abut on a tracker-page boundary; the previous single-match restriction rejected the second owner.

GPU invalidation now collects and retains all classified owners before applying the existing per-image transitions. All owner ranges must be nonzero, nonoverflowing, and pairwise disjoint at tracker-page granularity. Shared-page aliases still fail; each transition retains its existing buffer/image/coherence checks. Selection validation is not a transactional rollback guarantee. Captured pair, reverse order, overlap, shared edge page, zero and overflow tests passed, as did the full Windows build. Runtime candidate `audio-disjoint-owners-output` is being checked with actual Astro. No audible Astro PCM or recognizable native game frame is confirmed.

### 2026-10-28: actual disjoint-owner result and buffer-owned linear retirement

Actual Astro PID 22196 passed the former single-owner stop. The log records `MULTI-IMAGE-BUFFER-WRITE owners=2`, the full render-target readback, and `LINEAR-STORAGE-BUFFER-SYNC full_backing=1 padding_preserved=1 edge_isolated=1`. The run exited 321 at 18:41:04 after reusing that write range as a render target. New stop: requested `0x274e9b00000+0x480000`, linear storage `0x274e9e00000+0x3f4800`, native image GPU=false, buffer-owned=true, tracker GPU=false, CPU=false. This was a new transition after successful dual-owner publication.

The next candidate admits retirement only for the already supported linear UNorm layout after the requested target source is coherent, with no native/tracker GPU image owner, no CPU-dirty image state, matching context, a validated target footprint, and no other image/metadata page owner. The buffer cache retains authority for bytes outside the new target. Existing fence retention and retirement isolation remain in force; no stale image is downloaded over newer buffer contents. Tests cover captured geometry, all seven state/isolation failures, target footprint/range failures and wrong storage format. Production extraction tests and full native Windows build passed. Candidate `audio-linear-retire-output` also passes the AvPlayer decoder/ABI self-test. Its actual Astro run is pending; the separate AvPlayer-only run is still being observed.
### 2026-10-28: real Sony startup audio confirmed

The `audio-linear-retire-output` Astro run (PID 20676) passed the previous linear-storage transition repeatedly, finished `title_controller_ship` resource loading at guest elapsed 635.22s, resumed AvPlayer at 743.68s, and produced actual nonzero AudioOut2 bed PCM. First nonzero host sample report: push 39434, stereo peak 0.00048678, raw input peak 0.00127386. At push 39936: stereo peak 0.01917737, raw 0.04880875; subsequent cumulative total 803 nonzero output blocks, host_drops=0. The user independently reported hearing Sony's opening sound. This is actual game audio, distinct from the silent audio track in the opening MP4 and from standalone tests. Visible game output is still unconfirmed; a live window capture showed only black/dither.

The run exited 321 at 19:16:59 at a new containing render-target transition: target `0x2d56e030000+0x870000`, GPU-owned R32Float color-tiled storage `0x2d56e1e0000+0xc0000`, 480x270, pitch512, tile27, one mip; image/tracker GPU=true, CPU=false, no buffers. Complete storage readback now covers this validated 32-bit color footprint, preserves old padding with the existing Gen5 color permutation, clears image GPU ownership only after publication, and retains native objects to their fences. Producer drain, coherent target source, containment and retirement isolation are required. Captured positive/negative ownership/layout tests, exact tiled-byte/padding checks and the native Vulkan R32/packed/RGBA16 suite passed. A fresh real-game run is pending.

An opt-in `PSEMU_AVPLAYER_FRAME_PROBE=1` source-only luma log samples one decoded frame per 64 successes, at most 1024 frames. It does not change pixels or present a replacement image. This will distinguish decoder source content from Vulkan output without relying on unstable private ABI offsets. The prior read-only private-layout probe found no live process after the exit and read no frame.
### 2026-10-28: source-read residency for checked GPU address loads

The R32 candidate exited 321 at 19:51:47 after reaching a new checked GPU read failure at `0x15fb3b8f2e0`, status1/preparable1. Decoder source luma was nonblack (for example frame384, PTS6406, min16/max234, 165/1024 samples above32), while the new present#1220 framebuffer capture was still black. Actual game audio remains confirmed; later PCM also became nonzero beyond the Sony cue. A decoded source frame is not a visible Vulkan game frame.

Resource materialization now records only addresses successfully read by its existing SRT evaluator when checked guest-address translation is active. It merges adjacent dwords, keeps gaps, validates canonical aligned ranges and bounds the work to 65536 dword reads. No extra pointer chasing or speculative guest reads are introduced. Failed evaluation leaves the prior snapshot unchanged, including its read footprint. Non-address-table shaders keep an empty footprint.

Descriptor preparation establishes those exact source ranges as read-only, force-resident buffers through the ordinary cache coherence path before collecting native descriptor views. This ordering permits cache merges without invalidating already-collected views within the stage. Existing recording-generation retry, image/metadata exclusion, physical-load checks and fence retention remain in force. This addresses a missing bridge between successful CPU SRT reads and the resident-only GPU address table; it does not guarantee that every dynamically generated GPU address is now covered.

Fault completion retains the actual address-table ranges and logs shader hash/stage, GS root, prepared source-range count, CPU lookup result, host-mapped state and neighboring table ranges before the existing fatal error. Partial GPU output is still rejected; no missing read is silently replaced or replayed.

Tests passed: production GS-root materialization with an independent GPU scalar load, root refresh, missing-root and mid-read rollback, malformed footprint rejection, exact adjacent/gapped/duplicate read planning, canonical/address/byte-budget limits. Production SPIR-V lookup still agrees with the CPU resolver for 65536 ranges and 2000 randomized reads; missing/unaligned physical loads, indirect buffer bounds, BVH cases and Vulkan1.1 validation passed. The native Windows candidate `source-read-residency-output` built successfully. Actual Astro validation of the new residency bridge is pending.

### 2026-10-28: actual source-read candidate and video-path isolation

PID7788 (`source-read-residency-output`, source b0f6700) ran from 22:52:33 to 23:16:18 and exited with 0xc0000005. The first crash header reports guest RVA0x37f34c. Investigation found that the later access address and instruction-RVA fields used decimal formatting with a misleading `0x` prefix: decimal3666764 equals hexadecimal37f34c, and decimal144 equals hexadecimal90. Both reports describe the same `mov rax,[rax+0x90]` with RAX0, not different execution contexts. Those fields now explicitly select hexadecimal formatting; fault handling is unchanged. No checked GPU address-fault record or source-read-prepare log was emitted, so this run does not validate the residency change against the prior GPU failure. Do not skip/null-fill this guest fault. Actual AudioOut2 again reached803 nonzero output blocks and decoder source luma progressed through frame448/PTS7474.

Read-only sampling of logged video texture CPU pages found nonblack data in both 3840x3240 R8 sources (for example 578/1024 and583/1024 luma samples above32), separate from the decoder buffers. CPU-page sampling does not establish GPU ownership/coherence or uploaded image content. Framebuffer visibility is still unconfirmed.

The unmodified captured opening-video SPIR-V pair (VSefd4cb1c, PS7313c4a5) passed a new independent Vulkan graphics test for three synthetic NV12 colors on Arc B580, including complete framebuffer comparison and a clean Khronos validation run. This narrows investigation of opening-video blackness toward actual guest resources/draw/presentation state; it does not prove that other game shaders are correct or that an Astro game frame has appeared. See `astro-avplayer.md` for reproducible commands and limitations. A separate `video-binding-probe-output` candidate includes opt-in bounded binding/upload/draw/coverage diagnostics for the actual video path, without replacing guest pixels or changing default rendering.


### 2026-10-29: Compute entry roots missing from resident address table

The `video-target-luma-output` run (PID5524, source7f50dae) exited321 at00:09:27 on a checked Compute read at `0x1deab15f2e0`: shader `0x1dea818aa00`, stage4,34 table ranges, zero prepared source ranges, CPU resolver false, host mapping present. This is a distinct shader setup issue from the earlier guest null dereference. Read-only decoding of the owned1008-byte kernel found entry `s_load_dwordx4 s24,s0,+16`, `s_load_dwordx2 s28,s0,+0`, `s_load_dword s31,s0,+8`, and `s_load_dwordx8 s36,s14,+0`. The prior residency bridge could only prepare reads already performed by host SRT evaluation; these direct GPU reads can have no host descriptor consumer.

The next candidate adds exact read preparation only for Compute's predecessor-free entry block atPC0, using grouped scalar loads whose two reaching definitions are initial UserData SGPRs and whose offset is immediate. Descriptor sources use the existing checked GetDescriptorSource resolver; scalar value IDs use the evaluator's ordinary indexing. The planner follows the emitter's separate zero-extended dword alignment of instruction/component offsets, refreshes roots each dispatch and preserves a grouped load's pre-write provenance. It does not evaluate pointers or perform additional CPU guest reads. Conditional blocks, dynamic offsets, memory-derived roots, unknown sources and malformed/noncanonical addresses are not prefetched. Existing buffer-cache residency/coherence and fence lifetime handling are reused. Shader instructions and checked GPU fault behavior are unchanged; no zero fallback or partial replay is added. Tests and runtime validation are recorded separately when complete.


Validation: `test_guest_address_table.ps1` and `test_es_front_decode.ps1` passed with fresh production objects. The Compute fixture covers exact adjacent/gapped ranges, refreshed roots, pre-write grouped overlap, unknown sources, noncanonical addresses, non-Compute exclusion and conditional/dynamic exclusion, without planner-side guest reads. The existing GS-root/rollback and mesh ABI cases also passed. A full local Windows build completed successfully into `D:/Tools/psemu-build/compute-entry-roots-output`, with its matching map. Actual Compute-fault and downstream presentation validation still require the next real Astro run.


### 2026-10-29: include the live default SRT reader in source residency

A separate local test decoded the owned1008-byte failing Compute kernel and passed it through production CFG, IR lowering, scalar provenance, SRT patching, resource tracking and materialization. With two synthetic initial roots and a synthetic null image descriptor, the snapshot has exactly `[A,12]`, `[A+16,16]`, `[B,32]`. Both planner configurations perform the same8 existing host descriptor reads, so the additional entry footprint introduces no extra host reads. This is a CPU planning check with real instructions and synthetic resources; it is not a GPU dispatch or game frame.

That comparison exposed a gap in the previous host-source trace: it wrapped only explicitly supplied `SrtRuntime.read_memory` callbacks. Live `ShaderMaterializeStageRuntime` supplies no callback, so its existing guarded `ReadProcessMemory` reads were omitted. `ReadSrtRuntimeMemory` now shares the existing supplied/default-reader behavior between evaluator and trace. It preserves Windows complete-dword staging and the existing error/transactional behavior. The trace is enabled for guest-address-table programs with either reader; its existing budget is checked before reading and only successful reads are recorded. No extra pointer chase, new read or fallback value is introduced.

Fresh production guest-address/ES regressions pass, including a real local descriptor pointer with no callback, changed descriptor contents, exact16-byte footprint, failed default Windows read preserving its output canary, and failed materialization preserving prior buffers/footprint. Prior custom-reader, Compute entry, mesh ABI, root refresh and rollback cases remain passing. The separate `default-srt-reader-output` Windows candidate builds successfully with its matching map. The ongoing real Astro run still uses the earlier2d465ea candidate, to finish its observation window before switching candidates.


### 2026-10-29: real guest RW-lock support staged

The eb282be run again failed at guest RVA0x37f34c on a null Animation resource-node payload, before the new GPU array transition was observed. Read-only disassembly of the owned already-plain SELF segments confirms the node payload load at0x37f32f (`[r14+0x10]`) precedes the failing nested read. No crash-site branch or game data is patched. Some missing effect-path UI assets also exist in the actual ui directories; this alone does not justify redirecting paths or claiming an incomplete dump.

A separate missing API was verified in the active native route: `scePthreadRwlockattrInit/Init/attrDestroy/Rdlock/Wrlock/Unlock` have no core handler or registered native pthread target and therefore fall through to the success stub. Actual runtime logs call these APIs, including read/unlock calls near the previous crash. This permits concurrent accesses; causality for the Animation failure remains unproven.

New `PSEMU_NATIVE_RWLOCK=1` support owns real reader/writer state independently of the GPU library's different pthread identity system. It implements default attributes, initialization/destruction, lazy zero static handles, shared and recursive readers, exclusive writers with writer preference, ownership checks, try locks and relative-microsecond SCE timed locks. Existing readers may recurse while a writer waits. Destroy rejects owners or waiters; shared registry references and process-lifetime registry ownership avoid releasing state still used by host workers. Unsupported attribute variants and POSIX absolute timed-wait APIs are not claimed. The existing default launch behavior is preserved until the opt-in path is validated in Astro.

Reference: pinned Jetsku/KytyPS5 f9e19582f0deac0d1226a7ce3faae30a9ab408ab pthread read/write-lock ownership, wait and error behavior. This adapts that contract to the existing core memory validators and host thread identities; the whole foreign pthread runtime is not registered. Sony results use0x800200xx codes; POSIX wrappers return positive error numbers. Both native SysV PLT lookup and the VEH fallback call the same adapter.

The new pure registry/concurrency regression passes concurrent-reader acquisition,16000 mutually exclusive writes with paired-data invariants, recursive reads, blocked writer release, timed waits, invalid/foreign handles, wrong-owner unlock, busy/double destruction, default-attribute lifecycle and output guards. The built loader's `--rwlock-self-test` passes actual SysV Sony/POSIX targets, error results, handle writes and invalid-pointer validation. Both are included in the Windows workflow. The full separate D:/Tools/psemu-build/rwlock-output candidate builds; actual opt-in game validation is pending. This is a real synchronization implementation, not a proven fix for the null resource or final game presentation.


### 2026-10-29: bounded null-resource fault snapshot (opt-in)

`PSEMU_ASTRO_NULL_RESOURCE_PROBE=1` adds a read-only snapshot only to the existing terminal access-violation report at Astro RVA `0x37f34c` when RAX is null. It reads the component, matched resource node, and at most eight linked argument nodes with `ReadProcessMemory`, checked address arithmetic and cycle detection. Tags are limited to 32 readable printable bytes; non-null payload snapshots are limited to 64 bytes. Other guest threads are not frozen, so this is an observation rather than a consistent cross-thread snapshot or proof of the cause.

The probe does not alter guest memory, registers, instruction pointers, API results, or exception recovery. It is disabled by default. The separate D-drive `standard4kb-null-snapshot-output` candidate builds successfully and passes `--rwlock-self-test` (`tools/data/null_snapshot_rwlock_selftest.txt`). The already-running `2ad781c` Astro trial retains its original executable and does not contain this new diagnostic. Actual fault-snapshot execution remains unverified until a matching failure occurs with the opt-in flag enabled.
