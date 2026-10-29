# Native AvPlayer integration

AvPlayer uses the existing GPL-2.0 Kyty implementation with optional static FFmpeg dependencies. Ordinary builds do not download or enable it implicitly. The local package is the exact dependency pinned by the Jetsku reference: `KytyPS5/ext-ffmpeg-core` commit `9ac4cfd195f192ed8b08566f49c28dbb48d08341`. The Windows archive's GitHub release SHA256 is checked by the setup script. Its license/source notices are copied next to the built executable.

Reference: https://github.com/KytyPS5/ext-ffmpeg-core/blob/9ac4cfd195f192ed8b08566f49c28dbb48d08341/CMakeLists.txt
Release: https://github.com/KytyPS5/ext-ffmpeg-core/releases/tag/9ac4cfd195f1

Run in the existing portable toolchain environment, using only D: output locations:

```powershell
.\tools\scripts\setup_ffmpeg.ps1 -ToolsDirectory D:\Tools
cmake -S D:\proje\psemu -B D:\proje\psemu\build-local -DPSEMU_FFMPEG_ROOT=D:/Tools/ffmpeg-9ac4cfd195f1 -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=D:/Tools/psemu-build/audio-avplayer-output
cmake --build D:\proje\psemu\build-local --parallel 6
.\tools\scripts\test_avplayer.ps1 -LoaderPath D:\Tools\psemu-build\audio-avplayer-output\loader.exe -MediaPath D:\proje\psemu\PPSA21564-app\data\prein\video\ps_studio_armadillo.mp4 -ScratchDirectory D:\Tools\psemu-build\temp
```

The core dispatches actual `sceAvPlayer*` calls into the decoder. The bridge snapshots and validates input/output structures, bounds path/language reads, validates allocator and I/O callback sets, guards unknown/stale handles, retains player state across calls, and serializes operations per player. Guest allocation callbacks remain authoritative; their returned buffers must be writable before decoding. The guest must consume returned frames and route PCM through its own audio path. The bridge does not play audio directly or draw an external movie over the game window.

The real-file self-test calls the production bridge with System V ABI allocator, file, and event callbacks. It checks video metadata, frame/output canaries, InitEx output width, protected-page input/output boundaries, read-only outputs, invalid pointers/handles, complete allocation release, and a generated nonzero s16 WAV with exact expected samples. The scratch directory is an explicit argument; there is no implicit OS temp directory. It also decodes the source audio independently before conversion. In the owned Astro opening file, all 399 decoded source blocks are silent; production PCM is silent too. The decoder emitted 3840x2160 NV12 metadata and 1,634,304 stereo PCM bytes. This test is neither audible Astro playback nor a native Vulkan game frame.

The pinned package includes AAC and s16 PCM decoding, but not PCM float32 decoding or AAC encoding. The nonzero fixture therefore uses supported s16 PCM. HLS and arbitrary unsupported APIs/codecs still return explicit errors. Native guest callback behavior and subsequent Vulkan texture usage need actual game runs; standalone decoder success does not establish them.
## Actual guest result, 2026-10-28

Astro PID 18296 executed the real Init, PostInit, AddSource, StreamCount (2), GetStreamInfo, EnableStream, SetAvSyncMode, SetLooping and Start paths. Its own callbacks opened `data/prein/video/ps_studio_armadillo.mp4`. Actual `GetVideoDataEx` returned 1 with a 3840x2160 decoded frame at PTS 0; the guest then paused playback. The run still stopped at the previously identified buffer-owned linear-storage/render-target alias at 18:56:31. This proves real guest callback and decoder integration, but not visible Vulkan presentation or audible game sound.

CI now explicitly installs the pinned dependency, enables the decoder through `PSEMU_FFMPEG_ROOT`, requires its license/source notices in the package, and runs the production ABI/PCM test with a generated WAV. `test_avplayer.ps1` without MediaPath generates this private, temporary fixture in its explicit ScratchDirectory and removes it afterward. No game files are needed or uploaded. With MediaPath, the same test checks real video decoding too. Both paths passed locally; workflow YAML and its new PowerShell steps parsed successfully.
The subsequent `audio-linear-retire-output` run produced real nonzero AudioOut2 output and the user confirmed hearing Sony startup audio. Native decoder frames continued after Resume. This confirms audible game sound; native visible Vulkan game output is still unconfirmed. The former linear-target crash was passed; a later R32 color-storage/containing-target transition is the next runtime validation point. `PSEMU_AVPLAYER_FRAME_PROBE=1` enables bounded, read-only source luma diagnostics to separate decoded input from presentation.

### Opening-video shader replay (2026-10-28)

The captured production vertex `efd4cb1c` and pixel `7313c4a5` SPIR-V modules now run unchanged in an independent Vulkan graphics pipeline with synthetic 64x48 NV12 packed into an R8 image. The test checks every pixel in an 8x8 float framebuffer against CPU YUV-to-RGB expectations for black, neutral bright and colored inputs. Both a synthetic full-screen vertex shader plus the real pixel shader and the complete real vertex/pixel pair passed on Arc B580. Maximum component errors were 0.000003, 0.000384 and 0.000922 (limit 0.006, including the shader's half packing). Repeated with the Khronos validation layer: no validation errors or warnings after matching the production writable-buffer feature requirements in the test device.

Run from the portable build environment with explicit caller-owned captures:

```powershell
.\tools\scripts\test_native_video_shader_gpu.ps1 -VertexSpirv <production-video-vs.spv> -PixelSpirv <production-video-ps.spv>
```

The test logs input SHA-256 hashes and does not include proprietary shader bytes, game images or media. It exercises captured compiled modules, not recompilation of their source. It cannot prove that the running guest supplies the same valid resources, matrix, draw state or presentation target. It is not an Astro game-frame result and is intentionally outside asset-free CI.

`PSEMU_ASTRO_VIDEO_BINDING_PROBE=1` selects bounded runtime diagnostics for that captured shader pair: raw native snapshot buffer/image/sampler descriptors, actual upload-source luma samples, video draw target/mask/blend/depth/viewport/scissor and completed-fence occlusion results with the pixel-shader hash. The upload probe reads only bytes already being uploaded and does not read guest pointers, change ownership, replace pixels or add readbacks. Coverage queries add measurement GPU work only while explicitly enabled. Default rendering is unchanged.


Actual PID15524 video measurements (23:32 onward) confirm that the runtime SPIR-V hashes exactly match the replayed modules: VS SHA256 A0BD5F9E56113B860810A442E314022BE31B8DA8F31619B226D92D2D6361CC7A and PS 0BA9EE6742854B782C04552FDFD0B5A32CA7871069C923F4FCD4822086421982. At video draw64, the real upload source had399/1024 luma samples above32; draw320 had1014/1024. Completed occlusion queries reported8294400 samples for the3840x2160 target at draws64,128,256 and320. The draw uses6 vertices, one instance, full scissor, negative-height full viewport, mask0xf, blend disabled and depth test/write disabled. The offscreen video target differs from the registered VideoOut surfaces. CPU-only reads of logged constant buffers showed width3840, height3240, the expected matrix and embedded six-vertex rectangle; these reads do not prove native-buffer coherence. Present#1224 remained black on visual inspection.

The next opt-in diagnostic samples1024 texels from the actual native color attachment after selected video draws (0,64,128,256,448), with transfer-to-host visibility and completed-fence mapping. It immediately restores the original image layout, retains the readback allocation until completion and never publishes diagnostic bytes to guest backing. It also logs the native format and mapped write mask, and reports unsupported image shapes/formats explicitly. This adds GPU measurement work only under `PSEMU_ASTRO_VIDEO_BINDING_PROBE=1`; actual target-pixel results are pending runtime validation.

Target summaries also count positive RGB components above 0.02, positive peak and nonfinite values, decoding supported UNorm, half-float, RGB10 and unsigned RGB11 packed formats. This distinguishes near-black video range offsets from meaningful target brightness without changing any pixels.

The replay test was extended with an 8x8 luma checker pattern and a third pipeline configuration using the conventional six-vertex video rectangle, observed matrix shape and negative-height viewport. All four inputs passed in all three configurations, with no Khronos validation errors/warnings. The patterned case checks UV interpolation and orientation across every framebuffer pixel rather than relying only on uniform colors; maximum pattern component error was 0.000384. This remains a synthetic-source replay of real modules, separate from actual guest rendering.


Actual native target result (PID5524, source7f50dae, first result2026-10-28 23:59): video draw0 wrote all1024 sampled RGB pixels, positive peak0.141176477. Draw64 and128 reached peak1; draw448 had94/1024 samples above0.02 and peak1, with no nonfinite components. The native target is VkFormat37/RGBA8 UNorm at0x1df21120000; native write mask0xf. These are completed GPU draw readbacks, not decoder-only or CPU-backing samples. They establish nonzero changing native video-target content, but not its recognizable image or final VideoOut presentation. Initial registered VideoOut surfaces are0x1deaf070000 and0x1deb1050000, distinct from the target. The bounded texture log had62 unique entries and did not show that target as a sampled image; buffer-copy/compositor or unexecuted-link possibilities still need tracing.

The next candidate retains the same readback at video draw64 and, only for the observed3840x2160 RGBA8 shape, saves one full completed native target BMP under tools/dumps/native_video_target_<timestamp>.bmp. It never substitutes that target for presentation. A metadata-only trace opens for the following64 graphics draws and at most2048 buffer/image edges, recording shader hashes, requested guest addresses, native image identities, access flags and target addresses. This is intended to locate where the actual video target is consumed or disconnected; runtime capture/trace validation is pending.


### Recognizable native opening-video frame (2026-10-29)

The `video-graph-output` run (PID16632, source83417b4) completed video draw64 into the actual 3840x2160 RGBA8 Vulkan attachment at guest address `0x1b128050000`. Its completed-fence readback saved `tools/dumps/native_video_target_17932227901411762.bmp` (24,883,254 bytes). Visual inspection confirmed recognizable Astro imagery from the owned opening video. This is the output of the guest video shader in the real graphics pipeline, not a source decoder frame or synthetic shader replay. It is an intermediate native target: final VideoOut/window presentation and rendered gameplay remain unconfirmed. The BMP and owned shader/game data are local diagnostics and are not committed.

The process ended at00:27:14 with exit0xc000013a. The bounded graph trace contains11 subsequent graphics draws,186 image edges and188 buffer edges. None of those image edges references the video target. The promised64-draw observation window did not complete, so this is insufficient evidence to conclude that no later compositor consumes the target. The trace includes HDR/postprocessing and GE draws but did not establish the final video-to-VideoOut connection. Continue this trace in the next real run; do not present the intermediate attachment as a substitute for the game's output.


### Completed graph window and next actual stop (2026-10-29)

The2d465ea run (PID2032,11:42:20..12:10:44) again saved recognizable native video output to `tools/dumps/native_video_target_17932643045149858.bmp`. The64 following graphics draw headers completed; the general resource log reached its2048-edge limit (1014 images,1034 buffers, first8 slots per stage). No recorded image base or buffer range references target `0x24629280000`. This is a bounded observation, not proof that every possible consumer is absent. The live window and final present#1324 capture remained black. Real AudioOut2 again reached803 nonzero blocks.

The former checked Compute-address stop did not recur. Actual source preparation logged two ranges for each of shaders `0x245b02eaa00` and `0x245b02e5200`. The game logged `Level has started: title_controller_ship` at guest1695.74s, then stopped at `ValidateStorageImageResource`: unsigned storage image, dimension1/2D, read/write, atomic=true, mipNone. The launcher did not obtain a numeric exit code; the fatal diagnostic is confirmed. Do not classify this as the prior address-table failure or a successful game presentation.

A separate opt-in target watch now follows all descriptor slots and DMA/native image-copy/download operations for video draws64..128, with independent caps64 binding matches and128 transfer matches. It expires before normal movie teardown; native producer identity is refreshed and opaque handle reuse is explicitly called out in logs. Transfer identity alone is not ownership/pixel proof. Guest-address interval matches remain separately visible. No image contents, layouts, ownership, transfer selection or presentation behavior is replaced. Existing DMA regression passes; the local target-watch candidate builds successfully. Rejected storage resources additionally log their raw descriptor/format to identify the exact next unsupported case.
