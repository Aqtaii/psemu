$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
$first = $source.IndexOf('static bool SafeReadable(')
$last = $source.IndexOf('static std::string FormatSysVPrintf(', $first)
$memory = $source.Substring($first, $last - $first)
$first = $source.IndexOf('else if (readable_name == "sceKernelOpen"')
$last = $source.IndexOf('            } else if (readable_name == "fopen")', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Production file dispatch not found' }
$dispatch = $source.Substring($first, $last - $first)
$prefix = @"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>
#include <io.h>
#include "guest_file_stat.h"
std::atomic<uint64_t> g_vq_calls{0};
static std::mutex g_vfs_mutex;
static std::unordered_map<int, FILE*> g_kernel_files;
static std::string TranslateGuestPath(const std::string& p) { return p; }
static void EnsureParentDirs(const std::string&) {}
#define LOG_INFO(x) ((void)0)
"@
$wrapper = @"
static uint64_t Invoke(const std::string& readable_name, CONTEXT& state) {
    const std::string func_name = readable_name;
    CONTEXT* ctx = &state;
    bool special_return_set = false;
    if (false) {}
"@
$tests = @"
    }
    return ctx->Rax;
}
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    char temp[MAX_PATH]{}, path[MAX_PATH]{};
    CHECK(GetTempPathA(MAX_PATH,temp)); CHECK(GetTempFileNameA(temp,"pio",0,path));
    const char payload[] = "astro-data\n";
    CONTEXT ctx{}; ctx.Rdi = reinterpret_cast<uint64_t>(path); ctx.Rsi = 0x601;
    uint64_t writer = Invoke("sceKernelOpen",ctx); CHECK(writer >= 100 && writer != UINT64_MAX);
    ctx = {}; ctx.Rdi = writer; ctx.Rsi = reinterpret_cast<uint64_t>(payload); ctx.Rdx = sizeof(payload)-1;
    CHECK(Invoke("sceKernelWrite",ctx) == sizeof(payload)-1);
    ctx = {}; ctx.Rdi = writer; CHECK(Invoke("sceKernelClose",ctx) == 0);
    struct Guarded { uint64_t before; GuestFileStat stat; uint64_t canary; } guarded{};
    static_assert(offsetof(Guarded,canary) == 128);
    guarded.before = 0x0123456789abcdefULL; guarded.canary = 0xfedcba9876543210ULL;
    ctx = {}; ctx.Rdi = reinterpret_cast<uint64_t>(path); ctx.Rsi = reinterpret_cast<uint64_t>(&guarded.stat);
    CHECK(Invoke("sceKernelStat",ctx) == 0);
    CHECK(guarded.stat.size == sizeof(payload)-1 && (guarded.stat.mode & 0100000) != 0);
    CHECK(guarded.before == 0x0123456789abcdefULL && guarded.canary == 0xfedcba9876543210ULL);
    ctx = {}; ctx.Rdi = reinterpret_cast<uint64_t>(path);
    uint64_t reader = Invoke("sceKernelOpen",ctx); CHECK(reader >= 100 && reader != UINT64_MAX);
    ctx = {}; ctx.Rdi = reader; ctx.Rsi = reinterpret_cast<uint64_t>(&guarded.stat);
    CHECK(Invoke("sceKernelFstat",ctx) == 0 && guarded.stat.size == sizeof(payload)-1);
    CHECK(guarded.canary == 0xfedcba9876543210ULL);
    char buffer[64]{};
    ctx = {}; ctx.Rdi = reader; ctx.Rsi = reinterpret_cast<uint64_t>(buffer); ctx.Rdx = sizeof(buffer);
    CHECK(Invoke("sceKernelRead",ctx) == sizeof(payload)-1);
    CHECK(memcmp(buffer,payload,sizeof(payload)-1) == 0);
    CHECK(Invoke("sceKernelRead",ctx) == 0);
    ctx = {}; ctx.Rdi = reader; CHECK(Invoke("sceKernelClose",ctx) == 0);
    ctx = {}; ctx.Rdi = reader; ctx.Rsi = reinterpret_cast<uint64_t>(buffer); ctx.Rdx = sizeof(buffer);
    CHECK(Invoke("sceKernelRead",ctx) == UINT64_MAX);
    ctx = {}; ctx.Rdi = reader; ctx.Rsi = reinterpret_cast<uint64_t>(&guarded.stat);
    CHECK(static_cast<int32_t>(Invoke("sceKernelFstat",ctx)) == -2147352567);
    CHECK(guarded.canary == 0xfedcba9876543210ULL);
    CHECK(DeleteFileA(path));
    ctx = {}; ctx.Rdi = reinterpret_cast<uint64_t>(path); ctx.Rsi = reinterpret_cast<uint64_t>(&guarded.stat);
    CHECK(static_cast<int32_t>(Invoke("sceKernelStat",ctx)) == -2147352574);
    CHECK(g_kernel_files.empty());
    std::puts("PASS: PS5 120-byte stat preserves stack canary; actual kernel open/write/read/fstat/close share descriptors and return real bytes");
}
"@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'kernel_file_io_test.cpp'
$exe = Join-Path $out 'kernel_file_io_test.exe'
[IO.File]::WriteAllText($cpp, "$prefix`n$memory`n$wrapper`n$dispatch`n$tests", [Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /Od /EHsc /MD "/I$root\include" $cpp "/Fe:$exe" "/Fo:$out\kernel_file_io_test.obj"
if ($LASTEXITCODE -ne 0) { throw 'Kernel file test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Kernel file regression failed' }
