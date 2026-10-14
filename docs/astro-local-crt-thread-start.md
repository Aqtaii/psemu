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
