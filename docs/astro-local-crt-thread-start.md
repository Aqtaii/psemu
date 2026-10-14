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
