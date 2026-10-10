$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
$first = $source.IndexOf('static size_t HostAllocationSize(')
$last = $source.IndexOf('static PSEMU_SYSV void NativeFree(', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Native allocator not found' }
$production = $source.Substring($first, $last - $first)
$prefix = @"
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include "host_allocation_size.h"
#define PSEMU_SYSV
namespace Game { struct Profile { std::string title_id; }; Profile& Current() { static Profile p; return p; } }
static void RegisterAllocSize(void*,size_t) {}
"@
$tests = @"
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static SIZE_T Commit() { PROCESS_MEMORY_COUNTERS_EX p{}; p.cb=sizeof(p); if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p))) return 0; return p.PrivateUsage; }
int main() {
 CHECK(GuestHostAllocationSize(48,false)==48);
 CHECK(GuestHostAllocationSize(0,false)==16);
 CHECK(GuestHostAllocationSize(48,true)==65584);
 CHECK(GuestHostAllocationSize(0,true)==65536);
 CHECK(GuestHostAllocationSize(SIZE_MAX,true)==0);
 Game::Current().title_id="PPSA02929";
 _putenv_s("PSEMU_LEGACY_HOST_PADDING","1");
 CHECK(HostAllocationSize(48)==65584); CHECK(NativeMalloc(SIZE_MAX)==nullptr);
 _putenv_s("PSEMU_LEGACY_HOST_PADDING","0");
 CHECK(HostAllocationSize(48)==48);
 CHECK(NativeCalloc(SIZE_MAX,2)==nullptr);
 auto zeros=static_cast<unsigned char*>(NativeCalloc(64,4)); CHECK(zeros);
 for(int i=0;i<256;i++) CHECK(zeros[i]==0);
 _aligned_free(zeros);
 Game::Current().title_id="PPSA21564";
 std::vector<void*> allocations; allocations.reserve(10000);
 const SIZE_T before=Commit(); CHECK(before);
 for(int i=0;i<10000;i++) {
  auto p=static_cast<unsigned char*>(NativeMalloc(48)); CHECK(p);
  CHECK(reinterpret_cast<uintptr_t>(p)%16==0 && p[0]==0 && p[47]==0);
  p[0]=1; p[47]=2; allocations.push_back(p);
 }
 const SIZE_T after=Commit(); CHECK(after);
 const SIZE_T growth=after>before ? after-before : 0;
 std::printf("10000 actual NativeMalloc(48) allocations: commit growth %llu bytes\n",static_cast<unsigned long long>(growth));
 CHECK(growth<16*1024*1024);
 for(auto p:allocations) _aligned_free(p);
 auto zero=NativeMalloc(0); CHECK(zero); _aligned_free(zero);
 std::puts("PASS: Astro allocation footprint, zero initialization, alignment, overflow and Sarah padding");
}
"@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'host_allocation_size_test.cpp'
$exe = Join-Path $out 'host_allocation_size_test.exe'
[IO.File]::WriteAllText($cpp,"$prefix`n$production`n$tests",[Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /Od /EHsc /MD "/I$root\include" $cpp "/Fe:$exe" "/Fo:$out\host_allocation_size_test.obj" /link Psapi.lib
if ($LASTEXITCODE -ne 0) { throw 'Allocation test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Allocation memory regression failed' }