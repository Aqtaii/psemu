$ErrorActionPreference = 'Stop'
# Compile the actual production functions, not a second implementation.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
function Get-SourceRange([string]$begin, [string]$end) {
    $first = $source.IndexOf($begin, [StringComparison]::Ordinal)
    $last = $source.IndexOf($end, $first, [StringComparison]::Ordinal)
    if ($first -lt 0 -or $last -le $first) { throw "Production source range not found: $begin" }
    return $source.Substring($first, $last - $first)
}
$memory = Get-SourceRange 'static bool SafeReadable(' 'static size_t SafeStrlen('
$atomics = Get-SourceRange 'static PSEMU_SYSV int32_t NativeAtomicFetchAdd4(' 'static PSEMU_SYSV int NativeStrcmp('
$prefix = @'
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include "native_atomic_guard.h"
static std::atomic<uint64_t> g_vq_calls{0};
#define PSEMU_SYSV __attribute__((sysv_abi))
'@
$tests = @'
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static LONG WINAPI TestVeh(EXCEPTION_POINTERS* info) {
    if (NativeAtomicGuard::DeferException(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    std::puts("FAIL: atomic fault reached guest VEH logic");
    ExitProcess(2);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("STAGE valid atomics");
    auto* veh = AddVectoredExceptionHandler(1, TestVeh);
    CHECK(veh != nullptr);
    volatile int32_t value = 7;
    CHECK(NativeAtomicFetchAdd4(&value, 3) == 7 && value == 10);
    CHECK(NativeAtomicFetchSub4(&value, 4) == 10 && value == 6);
    CHECK(NativeAtomicLoad4(&value) == 6);
    int32_t expected = 6;
    CHECK(NativeAtomicCompareExchangeWeak4(&value, &expected, 9) == 1 && value == 9);
    expected = 3;
    CHECK(NativeAtomicCompareExchangeWeak4(&value, &expected, 11) == 0 && expected == 9 && value == 9);
    const auto queries = g_vq_calls.load();
    std::puts("STAGE concurrent atomics");
    volatile int32_t shared = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int j = 0; j < 100000; ++j) NativeAtomicFetchAdd4(&shared, 1);
    });
    for (auto& worker : workers) worker.join();
    CHECK(shared == 400000);
    CHECK(g_vq_calls.load() == queries);
    CHECK(!NativeAtomicGuard::active);
    std::puts("STAGE low pointers");
    for (uintptr_t address : {uintptr_t(0), uintptr_t(0xa), uintptr_t(0xfffc)}) {
        auto* bad = reinterpret_cast<volatile int32_t*>(address);
        CHECK(NativeAtomicFetchAdd4(bad, 1) == 0);
        CHECK(NativeAtomicFetchSub4(bad, 1) == 0);
        CHECK(NativeAtomicLoad4(bad) == 0);
        CHECK(NativeAtomicCompareExchangeWeak4(bad, &expected, 1) == 0);
        CHECK(NativeAtomicCompareExchangeWeak4(&value, const_cast<int32_t*>(bad), 1) == 0);
    }
    CHECK(g_vq_calls.load() == queries);
    std::puts("STAGE benchmark");
    LARGE_INTEGER frequency, begin, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < 100000; ++i) NativeAtomicFetchAdd4(&value, 1);
    QueryPerformanceCounter(&end);
    const double fast_ms = 1000.0 * (end.QuadPart - begin.QuadPart) / frequency.QuadPart;
    CHECK(g_vq_calls.load() == queries);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < 100000; ++i) {
        CHECK(SafeWritable(const_cast<int32_t*>(&value), sizeof(value)));
        _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(&value), 1);
    }
    QueryPerformanceCounter(&end);
    const double checked_ms = 1000.0 * (end.QuadPart - begin.QuadPart) / frequency.QuadPart;
    std::printf("BENCH 100k atomics: SEH %.3f ms, VirtualQuery %.3f ms\n", fast_ms, checked_ms);
    value = 9;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    auto* page = static_cast<int32_t*>(VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(page != nullptr);
    *page = 21;
    DWORD previous;
    CHECK(VirtualProtect(page, info.dwPageSize, PAGE_READONLY, &previous));
    std::puts("STAGE readonly memory");
    CHECK(NativeAtomicLoad4(page) == 21);
    CHECK(NativeAtomicFetchSub4(page, 1) == 0 && *page == 21);
    CHECK(NativeAtomicFetchAdd4(page, 1) == 0 && *page == 21);
    CHECK(NativeAtomicCompareExchangeWeak4(page, &expected, 1) == 0);
    CHECK(NativeAtomicCompareExchangeWeak4(&value, page, 1) == 0 && value == 9);
    CHECK(VirtualProtect(page, info.dwPageSize, PAGE_READWRITE, &previous));
    *page = value;
    CHECK(VirtualProtect(page, info.dwPageSize, PAGE_READONLY, &previous));
    CHECK(NativeAtomicCompareExchangeWeak4(&value, page, 123) == 0 && value == 9 && *page == 9);
    auto* second = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(page) + info.dwPageSize);
    std::puts("STAGE noaccess memory");
    CHECK(VirtualProtect(second, info.dwPageSize, PAGE_NOACCESS, &previous));
    CHECK(NativeAtomicLoad4(second) == 0);
    CHECK(NativeAtomicFetchSub4(second, 1) == 0);
    auto* boundary = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(second) - 2);
    CHECK(NativeAtomicLoad4(boundary) == 0);
    CHECK(NativeAtomicFetchAdd4(boundary, 1) == 0);
    std::puts("STAGE guard memory");
    CHECK(VirtualProtect(page, info.dwPageSize, PAGE_READWRITE | PAGE_GUARD, &previous));
    CHECK(NativeAtomicLoad4(page) == 0);
    CHECK(NativeAtomicFetchSub4(page, 1) == 0);
    std::puts("STAGE released memory");
    CHECK(VirtualFree(page, 0, MEM_RELEASE));
    CHECK(NativeAtomicLoad4(page) == 0);
    CHECK(NativeAtomicFetchAdd4(page, 1) == 0);
    CHECK(!NativeAtomicGuard::active);
    CHECK(RemoveVectoredExceptionHandler(veh));
    std::puts("PASS: valid atomics, low invalid pointers, readonly, noaccess, guard and page boundaries");
    return 0;
}
'@
$directory = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$file = Join-Path $directory 'native_atomic_test.cpp'
($prefix + "`n#include <initializer_list>`n" + $memory + "`n" + $atomics + "`n" + $tests) | Set-Content -LiteralPath $file -Encoding utf8
Push-Location $directory
try {
    & clang-cl /nologo /std:c++20 /Od /EHa /MD "/I$root\include" $file /Fe:native_atomic_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native atomic test compilation failed' }
    & '.\native_atomic_test.exe'
    if ($LASTEXITCODE -ne 0) { throw "Native atomic regression test failed, exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
