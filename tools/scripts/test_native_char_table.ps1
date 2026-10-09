$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
$first = $source.IndexOf('static constexpr auto g_native_char_table')
$last = $source.IndexOf('// ============================================================================', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Production character table not found' }
$production = $source.Substring($first, $last - $first)
$prefix = @'
#include <array>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#define PSEMU_SYSV __attribute__((sysv_abi))
'@
$tests = @'
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main() {
    const auto* table = static_cast<const uint8_t*>(NativeCharTable());
    CHECK(table != nullptr);
    for (size_t i = 0; i < 1024; ++i) {
        const uint8_t expected = i < 512 && (i % 2) == 0 ? static_cast<uint8_t>(i / 2) : 0;
        CHECK(table[i] == expected);
    }
    std::atomic<bool> valid{true};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) threads.emplace_back([&] {
        using TableFn = const void* (__attribute__((sysv_abi)) *)();
        volatile TableFn guest_call = &NativeCharTable;
        for (int n = 0; n < 10000; ++n) if (guest_call() != table) valid = false;
    });
    for (auto& thread : threads) thread.join();
    CHECK(valid.load());
    CHECK(NativeReturnZero() == 0);
    std::puts("PASS: HLE table byte equivalence, SysV call ABI, concurrent stable address");
}
'@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'native_char_table_test.cpp'
$exe = Join-Path $out 'native_char_table_test.exe'
[IO.File]::WriteAllText($cpp, "$prefix`n$production`n$tests", [Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /Od /EHsc /MD $cpp "/Fe:$exe" "/Fo:$out\native_char_table_test.obj"
if ($LASTEXITCODE -ne 0) { throw 'Character table test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Character table regression failed' }
