#define NOMINMAX
#include <windows.h>
#include "astro_tls_sites.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d error=%lu\n",__LINE__,GetLastError());return 1;}}while(0)
int main(){
 const auto& site=AstroTlsSites[0];std::array<uint8_t,28> context{};
 std::memcpy(context.data(),&site.before,8);std::memcpy(context.data()+8,AstroTlsInstruction.data(),12);std::memcpy(context.data()+20,&site.after,8);
 std::array<uint8_t,12> patch{},untouched{};untouched.fill(0xcc);
 CHECK(!PrepareAstroTlsPatch(nullptr,site,0x1480,&patch));CHECK(!PrepareAstroTlsPatch(context.data(),site,0x1480,nullptr));
 for(uint32_t bad:{0u,65536u}){patch=untouched;CHECK(!PrepareAstroTlsPatch(context.data(),site,bad,&patch));CHECK(patch==untouched);}
 for(size_t i=0;i<context.size();++i){auto changed=context;changed[i]^=1;patch=untouched;CHECK(!PrepareAstroTlsPatch(changed.data(),site,0x1480,&patch));CHECK(patch==untouched);}
 DWORD slot=TlsAlloc();CHECK(slot!=TLS_OUT_OF_INDEXES && slot<64);
 CHECK(PrepareAstroTlsPatch(context.data(),site,0x1480+slot*8,&patch));CHECK(patch[3]==0x65);
 auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));CHECK(code);
 std::memcpy(code,patch.data(),12);code[12]=0xc3;DWORD old=0;CHECK(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));CHECK(FlushInstructionCache(GetCurrentProcess(),code,13));
 using ReadTls=uintptr_t(*)();auto read=reinterpret_cast<ReadTls>(code);std::atomic<bool> good{true};
 std::vector<std::thread> threads;
 for(uintptr_t t=1;t<=8;++t)threads.emplace_back([&,t]{const uintptr_t value=0xabc00000+t*4096;if(!TlsSetValue(slot,reinterpret_cast<void*>(value)))good=false;for(int n=0;n<1000;++n)if(read()!=value)good=false;});
 for(auto& t:threads)t.join();CHECK(good.load());CHECK(TlsGetValue(slot)==nullptr);
 CHECK(VirtualFree(code,0,MEM_RELEASE));CHECK(TlsFree(slot));
 std::puts("PASS: actual patched GS instruction, eight thread-local values, all context mutations rejected, invalid slots unchanged; game frame=0");
}