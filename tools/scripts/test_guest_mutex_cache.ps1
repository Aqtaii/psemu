$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
$source=[IO.File]::ReadAllText("$root\src\core.cpp")
function Slice([string]$start,[string]$end){$a=$source.IndexOf($start);$b=$source.IndexOf($end,$a+1);if($a -lt 0 -or $b -le $a){throw "Missing production range: $start"};return $source.Substring($a,$b-$a)}
$prefix=@"
#define NOMINMAX
#include <windows.h>
#include "$root/include/guest_memory_query.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <set>
#include <array>
#include <cstdint>
#include <cstdio>
#define PSEMU_SYSV __attribute__((sysv_abi))
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static std::atomic<uint64_t> g_vq_calls{0};
"@
$production=$prefix+(Slice 'static bool SafeReadable(' 'static int CallGuestHdrToneMapLuminance(')+(Slice 'struct GuestMutex {' '// ============================================================================')+(Slice 'static PSEMU_SYSV int NativeMutexLock(' '// sceKernelWaitSema henuz')+[IO.File]::ReadAllText("$root\tests\guest_mutex_cache_test.cpp")
New-Item -ItemType Directory -Path $out -Force | Out-Null
[IO.File]::WriteAllText("$out\guest_mutex_cache_production.cpp",$production,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD guest_mutex_cache_production.cpp /Fe:guest_mutex_cache_test.exe
 if($LASTEXITCODE){throw 'Mutex production regression compilation failed'}
 & .\guest_mutex_cache_test.exe
 if($LASTEXITCODE){throw 'Mutex cache and SysV regression failed'}
} finally {Pop-Location}
