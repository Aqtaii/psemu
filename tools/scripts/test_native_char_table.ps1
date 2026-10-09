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
    const auto* upper = static_cast<const uint8_t*>(NativeUpperTable());
    CHECK(table != nullptr && upper != nullptr && upper != table);
    for (size_t i = 0; i < 1024; ++i) {
        const auto ch = i / 2;
        const uint8_t lower_expected = i < 512 && (i % 2) == 0 ?
            static_cast<uint8_t>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) : 0;
        const uint8_t upper_expected = i < 512 && (i % 2) == 0 ?
            static_cast<uint8_t>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch) : 0;
        CHECK(table[i] == lower_expected && upper[i] == upper_expected);
    }
    // Construct 2 dispatches function names without case sensitivity.
    const char* definition = "MenuReload";
    const char* call = "menuReload";
    for (int i = 0; definition[i]; ++i)
        CHECK(table[uint8_t(definition[i]) * 2] == table[uint8_t(call[i]) * 2]);
    CHECK(upper['a' * 2] == 'A' && upper['z' * 2] == 'Z');
    std::atomic<bool> valid{true};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) threads.emplace_back([&] {
        using TableFn = const void* (__attribute__((sysv_abi)) *)();
        volatile TableFn guest_call = &NativeCharTable;
        volatile TableFn upper_call = &NativeUpperTable;
        for (int n = 0; n < 10000; ++n) if (guest_call() != table || upper_call() != upper) valid = false;
    });
    for (auto& thread : threads) thread.join();
    CHECK(valid.load());
    CHECK(NativeReturnZero() == 0);
    std::puts("PASS: ASCII case maps, Construct 2 name matching, SysV ABI and concurrent stable addresses");
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
