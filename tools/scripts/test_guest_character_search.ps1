$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
$source=[IO.File]::ReadAllText("$root\src\core.cpp")
$a=$source.IndexOf('static const char* GuestFindCharacter(')
$b=$source.IndexOf('// VEH', $a)
if($a -lt 0 -or $b -le $a){throw 'Missing production character-search range'}
$prefix=@"
#define NOMINMAX
#include <windows.h>
#include "$root/include/guest_memory_query.h"
#include <atomic>
#include <algorithm>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#define PSEMU_SYSV __attribute__((sysv_abi))
static std::atomic<uint64_t> g_vq_calls{0};
"@
$production=$prefix+$source.Substring($a,$b-$a)+[IO.File]::ReadAllText("$root\tests\guest_character_search_test.cpp")
New-Item -ItemType Directory -Path $out -Force | Out-Null
[IO.File]::WriteAllText("$out\guest_character_search_production.cpp",$production,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /Od /EHsc /MD guest_character_search_production.cpp /Fe:guest_character_search_test.exe
 if($LASTEXITCODE){throw 'Character search production compilation failed'}
 & .\guest_character_search_test.exe
 if($LASTEXITCODE){throw 'Character search regression failed'}
} finally {Pop-Location}
