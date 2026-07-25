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
