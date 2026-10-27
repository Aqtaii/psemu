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