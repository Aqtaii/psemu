$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$bridge=[IO.File]::ReadAllText((Join-Path $root 'gpu\adapter\agc_bridge.cpp'))
$first=$bridge.IndexOf('extern "C" void PsemuRegisterGpuMemory(')
if($first -lt 0){throw 'GPU memory bridge missing'}
$production=$bridge.Substring($first)
$prefix=@"
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <chrono>
#include <thread>
#include "shared_direct_memory.h"
#include <cstdio>
#include "graphics/host_gpu/pageManager.h"
using namespace Libs::Graphics;
static unsigned phases=0;
static bool Handler(void*,PageFaultAccess,uint64_t,uint64_t,PageFaultPhase) noexcept { ++phases; return true; }
namespace Libs::Graphics {
struct Resources {
 PageManager pages{Handler,nullptr};
 void MapMemory(uint64_t v,uint64_t n,GpuAccess a){pages.OnGpuMap(v,n,a);}
 bool IsRegistered(uint64_t v,uint64_t n){return pages.IsRegistered(v,n);}
 bool HandleFault(PageFaultAccess a,uint64_t v){return pages.IsRegistered(v,1) && pages.HandleFault(a,v);}
};
struct Context { Resources resources; Resources* GetGpuResources(){return &resources;} };
static Context* g_render_ctx=nullptr;
}
"@
$tests=@"
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 CHECK(!PsemuHandleGpuMemoryFault(0,false));
 Context context; g_render_ctx=&context;
 auto mem=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)); CHECK(mem);
 const auto addr=reinterpret_cast<uint64_t>(mem); auto& pages=context.resources.pages;
 PsemuRegisterGpuMemory(addr,4096,3); CHECK(!pages.IsRegistered(addr,1));
 PsemuRegisterGpuMemory(addr,4096,0x33); CHECK(pages.IsRegistered(addr,4096));
 CHECK(pages.HasGpuAccess(addr,4096,GpuAccess::ReadWrite));
 PsemuRegisterGpuMemory(addr+4096,4096,0x12); CHECK(pages.HasGpuAccess(addr+4096,4096,GpuAccess::Read));

 pages.UpdatePageWatchers(true,addr,4096,PageWatchMode::ReadWrite);
 { PageManager::BackingWrite reserved(pages,addr,4096); pages.UpdatePageWatchers(false,addr,4096,PageWatchMode::ReadWrite); }
 mem[0]=42; CHECK(mem[0]==42);
 pages.UpdatePageWatchers(true,addr,4096,PageWatchMode::ReadWrite);
 CHECK(PsemuHandleGpuMemoryFault(addr,false)); CHECK(phases==3); CHECK(mem[0]==42);
 // After one real read resolution, repeated delayed reads must retain write watchers.
 pages.UpdatePageWatchers(true,addr,4096,PageWatchMode::Write);
 pages.UpdatePageWatchers(true,addr,4096,PageWatchMode::ReadWrite);
 CHECK(PsemuHandleGpuMemoryFault(addr,false)); CHECK(phases==6);
 MEMORY_BASIC_INFORMATION delayed{};
 CHECK(VirtualQuery(mem,&delayed,sizeof(delayed)) && delayed.Protect==PAGE_READONLY);
 for(int i=0;i<4;i++) CHECK(PsemuHandleGpuMemoryFault(addr,false));
 CHECK(phases==6);
 CHECK(VirtualQuery(mem,&delayed,sizeof(delayed)) && delayed.Protect==PAGE_READONLY);
 CHECK(PsemuHandleGpuMemoryFault(addr,true)); CHECK(phases==9); mem[0]=7;
 CHECK(mem[0]==7 && !PsemuHandleGpuMemoryFault(addr+16384,true));
 DWORD previous=0; CHECK(VirtualProtect(mem+4096,4096,PAGE_EXECUTE_READWRITE,&previous));
 pages.UpdatePageWatchers(true,addr,8192,PageWatchMode::Write);
 MEMORY_BASIC_INFORMATION mixed{};
 CHECK(VirtualQuery(mem,&mixed,sizeof(mixed)) && mixed.Protect==PAGE_READONLY);
 CHECK(VirtualQuery(mem+4096,&mixed,sizeof(mixed)) && mixed.Protect==PAGE_READONLY);
 pages.UpdatePageWatchers(false,addr,8192,PageWatchMode::Write);
 CHECK(VirtualQuery(mem,&mixed,sizeof(mixed)) && mixed.Protect==PAGE_READWRITE);
 CHECK(VirtualQuery(mem+4096,&mixed,sizeof(mixed)) && mixed.Protect==PAGE_EXECUTE_READWRITE);
 pages.OnGpuUnmap(addr,4096,GpuAccess::ReadWrite); pages.OnGpuUnmap(addr+4096,4096,GpuAccess::Read);
 CHECK(VirtualFree(mem,0,MEM_RELEASE));
 SharedDirectMemory shared; CHECK(shared.Initialize(0x400000000ULL));
 auto large=shared.GuestBase(); constexpr uint64_t bytes=32*1024*1024;
 CHECK(VirtualAlloc(large,bytes,MEM_COMMIT,PAGE_READWRITE)==large);
 const auto base=reinterpret_cast<uint64_t>(large); pages.OnGpuMap(base,bytes,GpuAccess::ReadWrite);
 const auto begin=std::chrono::steady_clock::now();
 pages.UpdatePageWatchers(true,base,bytes,PageWatchMode::Write);
 pages.UpdatePageWatchers(true,base+bytes/2,bytes/2,PageWatchMode::ReadWrite);
 pages.UpdatePageWatchers(false,base,bytes,PageWatchMode::Write);
 MEMORY_BASIC_INFORMATION info{};
 CHECK(VirtualQuery(large,&info,sizeof(info)) && info.Protect==PAGE_READWRITE);
 CHECK(VirtualQuery(large+bytes/2,&info,sizeof(info)) && info.Protect==PAGE_NOACCESS);
 pages.UpdatePageWatchers(false,base+bytes/2,bytes/2,PageWatchMode::ReadWrite);
 CHECK(VirtualQuery(large+bytes-1,&info,sizeof(info)) && info.Protect==PAGE_READWRITE);
 auto worker=[&](){for(int i=0;i<4;++i){pages.UpdatePageWatchers(true,base,bytes,PageWatchMode::Write);pages.UpdatePageWatchers(false,base,bytes,PageWatchMode::Write);}};
 std::thread a(worker),b(worker); a.join();b.join();
 large[0]=19;large[bytes-1]=23;CHECK(large[0]==19 && large[bytes-1]==23);
 pages.OnGpuUnmap(base,bytes,GpuAccess::ReadWrite);
 std::printf("32MB shared-view protection batches and concurrent overlap: %.1f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
 g_render_ctx=nullptr;
 std::puts("PASS: real PageManager GPU mapping permissions, backing write reservation and read/write fault resolution");
}
"@
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$cpp=Join-Path $out 'gpu_memory_bridge_test.cpp';$exe=Join-Path $out 'gpu_memory_bridge_test.exe'
[IO.File]::WriteAllText($cpp,"$prefix`n$production`n$tests",[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /Od /EHsc /MD "/I$root\include" "/I$root\gpu\src" "/I$root\gpu\generated" $cpp "$root\gpu\src\graphics\host_gpu\pageManager.cpp" "/Fe:$exe"
 if($LASTEXITCODE -ne 0){throw 'GPU memory bridge compilation failed'}
 & $exe
 if($LASTEXITCODE -ne 0){throw 'GPU memory bridge regression failed'}
} finally {Pop-Location}