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