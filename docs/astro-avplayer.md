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
