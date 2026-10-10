# PS5 Emulator (psemu) Developer & AI Guide

This document defines workspace conventions, file organization rules, log formatting standards, and debugging procedures for developers and AI assistants working on `psemu`.

---

## 1. Directory Structure Guidelines (FOR ALL DEVELOPERS & AIs)

To prevent root directory clutter, maintain the following directory layout at all times. **DO NOT write temporary scripts, dump files, or text logs directly to the root directory.**

```
psemu/
├── src/                    # Core emulator source (CPU, kernel, SysV ABI, HLE hooks, loader)
├── gpu/                    # GPU subsystem (Vulkan host renderer, PM4 guest command processor, SPIR-V/Shaders)
├── include/                # Global headers and system-wide definitions
├── tools/                  # Workspace utility files (ORGANIZED)
│   ├── scripts/            # Python helper scripts (disasm, NID verification, brute force)
│   ├── dumps/              # Memory/register binary dumps (.bin)
│   └── data/               # Reference text databases (NID tables, disassembly dumps)
├── PPSA02929-app0/         # Sample PS5 game app directory
├── _Shaders/               # Compiled Vulkan shader cache
├── CMakeLists.txt          # Root CMake build specification
├── cmbuild.bat             # Fast build script (ninja + MSVC x64)
├── run.bat                 # Game boot execution script
├── HANDOFF.md              # Previous task handoff notes
└── DEVELOPER_GUIDE.md      # This guide
```

### Folder Rules for AI Assistants:
* **Python Scripts:** Save helper/scratch python scripts inside `tools/scripts/`.
* **Binary Dumps:** Save memory/register dumps in `tools/dumps/`.
* **Data Files:** Put NID databases or disassembly text outputs in `tools/data/`.
* **Temporary Artifacts:** Do NOT leave loose `.bin`, `.py`, or `.txt` files in the root folder.

---

## 2. Logging System Tutorial

Log readability is critical for performance and debugging. `psemu` uses categorized log prefixes.

### Log Tag Reference
| Tag | Component | Description |
|---|---|---|
| `[KERNEL]` | Core Kernel | FreeBSD kernel syscalls, thread creation, event queues (`eventQueue.cpp`) |
| `[GPU]` / `RenderColorTarget` | Vulkan Renderer | Render targets, framebuffers, draw calls, primitive topologies (`renderDraw.cpp`) |
| `[PRX-HLE]` / `[PLT-HOOK]` | HLE & PLT | Library function stubs, NID resolution, return values |
| `[VFS]` / `[GAME-LOG]` | File System & Logs | Guest path translation (`/app0/`), game `printf` outputs |

### High-Frequency Log Rate-Limiting Rule
**CRITICAL:** Never place unthrottled `printf` or `LOGF` calls inside per-draw or per-frame loops (such as PM4 packet handlers or Vulkan state setup).

When adding diagnostic warnings for repetitive GPU/Kernel states, **always** use atomic rate-limiters:

```cpp
// Example: Rate-limiting diagnostic log to 16 occurrences
static std::atomic<uint32_t> log_count {0};
if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
    LOGF("[GPU] Warning: unhandled register configuration 0x%08x\n", reg_val);
}
```

### Interpreting Crash Logs
When a crash occurs, look for `--- Fatal Error ---` at the end of `loader_log.txt`:
1. **Assertion failure:** Indicates an unhandled hardware mode or buffer size mismatch (e.g. `video_image.size < size`).
2. **PLT-HOOK Return 0:** Indicates a missing HLE function return value causing a downstream null-pointer dereference or infinite loop.

---

## 3. Key Architecture Rules

1. **PS4/PS5 Quad Primitives (`kRectListLegacy` / Primitive Type 17):**
   - PS4 games (especially GameMaker) draw UI quads using `kRectList` (3 vertices passed by the game).
   - In `renderDraw.cpp`, `kRectListLegacy` must be mapped to `eTriangleStrip` with `draw_vertex_count = 4` so Vulkan rasterizes a full rectangle instead of cutting the quad diagonally.

2. **HLE String/Memory Functions (`wmemchr`, `wmemmove`):**
   - GameMaker uses `libSceLibcInternal` HLE calls for text localization parsing.
   - Standard PLT stubs returning `0` will cause string parsers to freeze or return empty text. Always provide valid HLE pointer search fallbacks in `src/core.cpp`.

## Build entry points

Run `build.bat` or `cmbuild.bat` from any directory. Both use the same CMake/Ninja build, including the GPU library, and propagate failures to the caller.

Required: Windows x64, Visual Studio C++ tools, clang-cl, CMake, Ninja, and a Vulkan SDK with SPIRV-Tools headers and libraries. Put CMake, Ninja, and clang-cl on PATH and set VULKAN_SDK to the installed SDK directory. The script preserves that SDK selection.

Visual Studio is discovered through vswhere. For a custom installation, set PSEMU_VCVARS64 to the full path of vcvars64.bat, or run from an x64 Developer Command Prompt. Additional build arguments are forwarded to cmake --build; for example: cmbuild.bat --verbose.

The core source list is explicit in CMakeLists.txt; add new src/*.cpp files there. Core optimization remains disabled because the existing low-level runtime depends on it.

## KytyPS5 reference workflow

For Astro Bot and subsequent compatibility fixes, inspect https://github.com/KytyPS5/KytyPS5 before introducing an emulator workaround. Compare decoder, IR translation, SPIR-V emitter, PM4 handling, HLE ABI and upstream regression tests. Local checkout: D:/proje/psemu-codex/work/kytyps5-reference; include src/graphics, src/kernel, src/libs, src/loader, tests and docs in its sparse checkout.

Record the upstream commit and exact source/test paths in docs/kytyps5-reference.md. Adapt to psemu's ownership, command recording and guest ABI contracts. Preserve copyright notices and applicable licenses for directly imported source. Add a regression based on captured guest instructions/commands and upstream edge cases. Build and test the integrated emulator before claiming runtime progress. Store downloads, extracted builds and diagnostics on D:.
