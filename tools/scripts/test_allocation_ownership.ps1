$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
$first = $source.IndexOf('static std::mutex             g_alloc_size_mutex;')
$last = $source.IndexOf('// NID ONEK INDEKSI', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Allocation ownership source not found' }
$production = $source.Substring($first, $last - $first)
$prefix = @'
#include <windows.h>
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
int main() {
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
    std::puts("PASS: exact host ownership, guest pool rejection, stale/offset pointers, zero size, concurrent release");
}
'@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'allocation_ownership_test.cpp'
$exe = Join-Path $out 'allocation_ownership_test.exe'
$object = Join-Path $out 'allocation_ownership_test.obj'
[IO.File]::WriteAllText($cpp, "$prefix`n$production`n$tests", [Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /EHsc /MD $cpp "/Fe:$exe" "/Fo:$object"
if ($LASTEXITCODE -ne 0) { throw 'Ownership regression test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Ownership regression test failed' }
