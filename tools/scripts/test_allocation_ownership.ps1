$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
$first = $source.IndexOf('static std::mutex             g_alloc_size_mutex;')
$last = $source.IndexOf('// NID ONEK INDEKSI', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Allocation ownership source not found' }
$arenaFirst = $source.IndexOf('struct MspaceInfo {')
$arenaLast = $source.IndexOf('static uint8_t* DmemBase()', $arenaFirst)
if ($arenaFirst -lt 0 -or $arenaLast -le $arenaFirst) { throw 'Mapped allocation source not found' }
$production = $source.Substring($first, $last - $first) + $source.Substring($arenaFirst, $arenaLast - $arenaFirst)
$prefix = @'
#include <windows.h>
#include "mspaceArena.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <malloc.h>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>
'@
$tests = @'
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const bool reuse = argv[1][0] == '1';
    void* host = _aligned_malloc(128, 16);
    CHECK(host != nullptr);
    RegisterAllocSize(host, 128);
    CHECK(LookupAllocSize(host) == 128);
    CHECK(!FreeHostAllocation(static_cast<char*>(host) + 16));
    CHECK(FreeHostAllocation(host));
    CHECK(LookupAllocSize(host) == 0);
    CHECK(!FreeHostAllocation(host));
    CHECK(!FreeHostAllocation(nullptr));
    char guest[128] = {};
    RegisterAllocSize(guest, 128, false);
    CHECK(!FreeHostAllocation(guest));
    CHECK(LookupAllocSize(guest) == 128);
    host = _aligned_malloc(16, 16);
    RegisterAllocSize(host, 0);
    CHECK(FreeHostAllocation(host));
    host = _aligned_malloc(128, 16);
    RegisterAllocSize(host, 128);
    std::atomic<int> released{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) threads.emplace_back([&] { if (FreeHostAllocation(host)) ++released; });
    for (auto& thread : threads) thread.join();
    CHECK(released == 1);
    constexpr uint64_t capacity = 4u * 1024u * 1024u;
    auto* backing = static_cast<char*>(_aligned_malloc(capacity, 0x10000));
    CHECK(backing != nullptr);
    const auto base = reinterpret_cast<uint64_t>(backing);
    MspaceRemember(2, base, capacity);
    MspaceRemember(3, base, capacity / 2);
    CHECK(MspaceCapacity(2) == capacity && MspaceCapacity(3) == capacity / 2);
    CHECK(MspaceBumpAlloc(3, 32, 16) == nullptr);
    auto* mapped = MspaceBumpAlloc(2, 0x40000, 0x10000);
    CHECK(mapped == backing);
    RegisterAllocSize(mapped, 0x40000, false);
    CHECK(!FreeHostAllocation(backing + 1));
    CHECK(FreeHostAllocation(mapped) == reuse);
    CHECK(!FreeHostAllocation(mapped));
    const auto [peak, used] = MspaceUsage(2);
    CHECK(peak == 0x40000 && used == (reuse ? 0 : 0x40000));
    CHECK(LookupAllocSize(mapped) == (reuse ? 0 : 0x40000));
    CHECK(MspaceBumpAlloc(2, 0x40000, 0x10000) == backing + (reuse ? 0 : 0x40000));
    _aligned_free(backing);

    if (reuse) {
        constexpr uint64_t synthetic_base = 0x100000000ull;
        MspaceRemember(4, synthetic_base, capacity);
        MspaceRemember(5, synthetic_base + 0x10000, capacity);
        MspaceRemember(6, synthetic_base + capacity, capacity);
        MspaceRemember(7, UINT64_MAX - 15, 64);
        CHECK(MspaceBumpAlloc(5, 32, 16) == nullptr);
        CHECK(reinterpret_cast<uint64_t>(MspaceBumpAlloc(6, 32, 16)) == synthetic_base + capacity);
        CHECK(MspaceBumpAlloc(7, 32, 16) == nullptr);

        MspaceArena arena(synthetic_base, capacity);
        std::vector<uint64_t> fragments;
        for (size_t i = 0; i < 12000; ++i) {
            const auto addr = arena.Allocate(17, 64);
            CHECK(addr != 0 && addr % 64 == 0);
            fragments.push_back(addr);
        }
        const auto aligned = arena.Allocate(0x18000, 4096);
        CHECK(aligned != 0 && aligned % 4096 == 0);
        CHECK(arena.Release(aligned) && !arena.Release(aligned));
        for (const auto addr : fragments) CHECK(arena.Release(addr));
        CHECK(arena.Used() == 0 && arena.Allocate(capacity, 16) == 0x100000000ull);
    }
    std::printf("PASS: exact host ownership, concurrent release and mapped arena mode %d\n", int(reuse));
}
'@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'allocation_ownership_test.cpp'
$exe = Join-Path $out 'allocation_ownership_test.exe'
$object = Join-Path $out 'allocation_ownership_test.obj'
[IO.File]::WriteAllText($cpp, "$prefix`n$production`n$tests", [Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /EHsc /MD /D_CRT_SECURE_NO_WARNINGS "/I$root\src" $cpp "/Fe:$exe" "/Fo:$object"
if ($LASTEXITCODE -ne 0) { throw 'Ownership regression test compilation failed' }
$previousReuse = $env:PSEMU_MSPACE_REUSE
try {
    $env:PSEMU_MSPACE_REUSE = $null
    & $exe 0
    if ($LASTEXITCODE -ne 0) { throw 'Ownership regression test failed' }
    $env:PSEMU_MSPACE_REUSE = '1'
    & $exe 1
    if ($LASTEXITCODE -ne 0) { throw 'Mapped arena regression test failed' }
} finally {
    $env:PSEMU_MSPACE_REUSE = $previousReuse
}
