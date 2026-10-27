#define NOMINMAX
#include "shared_direct_memory.h"
#include <cstdio>
#include <array>
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line %d: %s error=%lu\n",__LINE__,#x,GetLastError()); return 1; } } while(0)
int main() {
 SharedDirectMemory memory;
 constexpr uint64_t capacity=1024*1024;
 CHECK(memory.Initialize(capacity));
 auto guest=memory.GuestBase();CHECK(guest);
 CHECK(VirtualAlloc(guest+65536,8192,MEM_COMMIT,PAGE_EXECUTE_READWRITE)==guest+65536);
 guest[65536]=0x67;
 CHECK(VirtualAlloc(guest+65536,8192,MEM_COMMIT,PAGE_EXECUTE_READWRITE)==guest+65536);
 CHECK(guest[65536]==0x67);
 const auto address=reinterpret_cast<uint64_t>(guest+65536);
 std::array<unsigned char,8192> source{},readback{};
 for(size_t i=0;i<source.size();++i) source[i]=static_cast<unsigned char>(i*17+3);
 DWORD old=0;
 CHECK(VirtualProtect(guest+65536,8192,PAGE_NOACCESS,&old));
 CHECK(memory.TryWrite(address,source.data(),source.size()));
 CHECK(memory.TryRead(address,readback.data(),readback.size()));CHECK(source==readback);
 MEMORY_BASIC_INFORMATION info{}; CHECK(VirtualQuery(guest+65536,&info,sizeof(info))); CHECK(info.Protect==PAGE_NOACCESS);
 CHECK(VirtualProtect(guest+65536,8192,PAGE_READONLY,&old));
 CHECK(std::memcmp(guest+65536,source.data(),source.size())==0);
 source[0]=99;CHECK(memory.TryWrite(address,source.data(),source.size()));CHECK(guest[65536]==99);
 CHECK(VirtualQuery(guest+65536,&info,sizeof(info)));CHECK(info.Protect==PAGE_READONLY);
 CHECK(!memory.TryWrite(address+8192,source.data(),1));
 CHECK(!memory.TryRead(address+8192,readback.data(),1));
 CHECK(!memory.TryWrite(address,source.data(),8193));CHECK(guest[65536]==99);
 CHECK(!memory.TryWrite(reinterpret_cast<uint64_t>(guest)+capacity-4,source.data(),8));
 CHECK(!memory.TryRead(UINT64_MAX-2,readback.data(),8));
 CHECK(!memory.TryWrite(1,source.data(),1));CHECK(!memory.TryWrite(address,nullptr,1));
 std::puts("PASS: actual shared section commits, NOACCESS/READONLY guest protection preserved, exact backing read/write, uncommitted and overflow rejection");
}