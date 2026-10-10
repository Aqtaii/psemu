$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
function Get-SourceRange([string]$begin, [string]$end) {
    $first = $source.IndexOf($begin, [StringComparison]::Ordinal)
    $last = $source.IndexOf($end, $first, [StringComparison]::Ordinal)
    if ($first -lt 0 -or $last -le $first) { throw "Source range not found: $begin" }
    return $source.Substring($first, $last - $first)
}
$memory = Get-SourceRange 'static bool SafeReadable(' 'static size_t SafeStrlen('
$dispatch = Get-SourceRange '            if (readable_name == "sceKernelCreateSema" ||' '            // ========================================================'
$prefix = @'
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <initializer_list>
#include "nids.h"
static std::atomic<uint64_t> g_vq_calls{0};
'@
$wrapper = @'
static LONG Dispatch(const std::string& readable_name, CONTEXT* ctx) {
'@
$tests = @'
    return EXCEPTION_CONTINUE_SEARCH;
}
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static bool Invoke(const std::string& name, CONTEXT& ctx) {
    uint64_t ret = 0x1234;
    ctx.Rsp = reinterpret_cast<uint64_t>(&ret);
    return Dispatch(name, &ctx) == EXCEPTION_CONTINUE_EXECUTION && ctx.Rip == ret;
}
int main() {
    struct Entry { const char* nid; const char* function; };
    for (const auto& entry : {
        Entry{"188x57JYp0g", "sceKernelCreateSema"},
        Entry{"R1Jvn8bSCW8", "sceKernelDeleteSema"},
        Entry{"Zxa0VhQVTsk", "sceKernelWaitSema"},
        Entry{"12wOHk8ywb0", "sceKernelPollSema"},
        Entry{"4czppHBiriw", "sceKernelSignalSema"},
        Entry{"4DM06U2BNEY", "sceKernelCancelSema"}}) {
        for (const char* suffix : {"#T#T", "#S#N", "#T#N", "#S#T"})
            CHECK(g_nid_to_name.at(std::string(entry.nid) + suffix) == entry.function);
    }
    uint64_t handle = 0;
    CONTEXT ctx{};
    ctx.Rdi = reinterpret_cast<uint64_t>(&handle);
    ctx.Rcx = 0;
    ctx.R8 = 64;
    CHECK(Invoke(g_nid_to_name.at("188x57JYp0g#T#T"), ctx) && ctx.Rax == 0 && handle != 0);
    ctx = {};
    ctx.Rdi = handle;
    ctx.Rsi = 1;
    CHECK(Invoke(g_nid_to_name.at("4czppHBiriw#S#N"), ctx) && ctx.Rax == 0);
    // The old mapping executed a 50-ms wait and left this semaphore empty.
    CHECK(WaitForSingleObject(reinterpret_cast<HANDLE>(handle), 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(reinterpret_cast<HANDLE>(handle), 0) == WAIT_TIMEOUT);
    CHECK(Invoke("sceKernelSignalSema", ctx) && ctx.Rax == 0);
    CHECK(Invoke("sceKernelWaitSema", ctx) && ctx.Rax == 0);
    CHECK(Invoke("sceKernelPollSema", ctx) && ctx.Rax != 0);
    std::atomic<DWORD> worker_result{WAIT_FAILED};
    std::thread worker([&] { worker_result = WaitForSingleObject(reinterpret_cast<HANDLE>(handle), 1000); });
    CHECK(Invoke("sceKernelSignalSema", ctx) && ctx.Rax == 0);
    worker.join();
    CHECK(worker_result == WAIT_OBJECT_0);
    uint32_t timeout_us = 1000;
    ctx.Rdx = reinterpret_cast<uint64_t>(&timeout_us);
    CHECK(Invoke("sceKernelWaitSema", ctx) && static_cast<int32_t>(ctx.Rax) == -2147352516);
    ctx.Rdx = 0;
    std::atomic<bool> started{false}, completed{false}, passed{false};
    std::thread real_waiter([&] {
        CONTEXT wait{}; wait.Rdi = handle; wait.Rsi = 1;
        started.store(true);
        passed.store(Invoke("sceKernelWaitSema", wait) && wait.Rax == 0);
        completed.store(true);
    });
    while (!started.load()) Sleep(1);
    Sleep(100);
    const bool premature = completed.load();
    CHECK(Invoke("sceKernelSignalSema", ctx) && ctx.Rax == 0);
    real_waiter.join();
    CHECK(!premature && completed.load() && passed.load());
    CHECK(CloseHandle(reinterpret_cast<HANDLE>(handle)));
    std::puts("PASS: six semaphore NIDs across four suffixes, actual signal/wait/poll handler and cross-thread wakeup");
}
'@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'semaphore_dispatch_test.cpp'
$exe = Join-Path $out 'semaphore_dispatch_test.exe'
[IO.File]::WriteAllText($cpp, "$prefix`n$memory`n$wrapper`n$dispatch`n$tests", [Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /Od /EHsc /MD "/I$root\include" $cpp "/Fe:$exe" "/Fo:$out\semaphore_dispatch_test.obj"
if ($LASTEXITCODE -ne 0) { throw 'Semaphore regression compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Semaphore regression failed' }
