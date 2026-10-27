#define NOMINMAX
#include "shared_direct_memory.h"
#include <cstdio>
#include <array>
#include <thread>
#include <atomic>
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line %d: %s error=%lu\n",__LINE__,#x,GetLastError()); return 1; } } while(0)
int main(int argc, char** argv) {
 SharedDirectMemory memory;
 const uint64_t capacity=argc>2?0x400000000ULL:1024*1024;
 const bool file_mode = argc > 1;
 wchar_t directory[MAX_PATH]{};
 if (file_mode) {CHECK(GetEnvironmentVariableW(L"TEMP",directory,MAX_PATH));CHECK(memory.InitializeFileBacked(capacity,directory));}
 else CHECK(memory.Initialize(capacity));
 auto guest=memory.GuestBase();CHECK(guest);
 if (argc>2) {
  MEMORYSTATUSEX before{},after{};before.dwLength=sizeof(before);after.dwLength=sizeof(after);
  CHECK(GlobalMemoryStatusEx(&before));
  CHECK(memory.Commit(0x100000,0x280000000ULL));
  CHECK(GlobalMemoryStatusEx(&after));
  CHECK(after.ullAvailPageFile + 512ULL*1024*1024 >= before.ullAvailPageFile);
  CHECK(guest[0x100000]==0 && guest[0x2800fffffULL]==0);
  std::puts("PASS: 16GiB sparse file, 10GiB logical allocation without 10GiB system commit");
 }
 CHECK(memory.Commit(65536,8192));
 guest[65536]=0x67;
 CHECK(memory.Commit(65536,8192));
 CHECK(guest[65536]==0x67);
 const auto address=reinterpret_cast<uint64_t>(guest+65536);
 std::array<unsigned char,8192> source{},readback{};
 for(size_t i=0;i<source.size();++i) source[i]=static_cast<unsigned char>(i*17+3);
 DWORD old=0;
 CHECK(VirtualProtect(guest+65536,8192,PAGE_NOACCESS,&old));
 CHECK(memory.Commit(65536,8192));
 CHECK(memory.TryWrite(address,source.data(),source.size()));
 CHECK(memory.TryRead(address,readback.data(),readback.size()));CHECK(source==readback);
 MEMORY_BASIC_INFORMATION info{}; CHECK(VirtualQuery(guest+65536,&info,sizeof(info))); CHECK(info.Protect==PAGE_NOACCESS);
 CHECK(VirtualProtect(guest+65536,8192,PAGE_READONLY,&old));
 CHECK(std::memcmp(guest+65536,source.data(),source.size())==0);
 source[0]=99;CHECK(memory.TryWrite(address,source.data(),source.size()));CHECK(guest[65536]==99);
 CHECK(VirtualQuery(guest+65536,&info,sizeof(info)));CHECK(info.Protect==PAGE_READONLY);
 CHECK(memory.Commit(65536,8192));
 CHECK(VirtualQuery(guest+65536,&info,sizeof(info)));CHECK(info.Protect==PAGE_READONLY);
 CHECK(!memory.TryWrite(address+8192,source.data(),1));
 CHECK(!memory.TryRead(address+8192,readback.data(),1));
 CHECK(!memory.TryWrite(address,source.data(),8193));CHECK(guest[65536]==99);
 CHECK(!memory.TryWrite(reinterpret_cast<uint64_t>(guest)+capacity-4,source.data(),8));
 CHECK(!memory.TryRead(UINT64_MAX-2,readback.data(),8));
 CHECK(!memory.TryWrite(1,source.data(),1));CHECK(!memory.TryWrite(address,nullptr,1));
 CHECK(memory.Commit(65536,12288));
 CHECK(VirtualQuery(guest+65536,&info,sizeof(info)));CHECK(info.Protect==PAGE_READONLY);
 CHECK(guest[65536]==99);CHECK(guest[65536+8192]==0);
 CHECK(!memory.Commit(capacity-4096,8192));CHECK(!memory.Commit(UINT64_MAX-1,2));
 CHECK(!memory.Commit(0,0));
 // Allocation coverage must preserve holes and merge adjacent/overlapping commits.
 const auto base=reinterpret_cast<uint64_t>(guest);
 CHECK(memory.Commit(131073,1));
 CHECK(memory.Commit(143360,4096));
 CHECK(memory.TryWrite(base+131072,source.data(),1));
 CHECK(!memory.TryRead(base+135168,readback.data(),1));
 CHECK(!memory.TryWrite(base+131072,source.data(),16384));
 CHECK(memory.Commit(135168,8192));
 CHECK(memory.TryWrite(base+131072,source.data(),8192));
 CHECK(memory.TryRead(base+131072,readback.data(),8192) && readback==source);
 CHECK(memory.Commit(139264,8192));
 CHECK(memory.TryRead(base+147455,readback.data(),1));
 CHECK(!memory.TryRead(base+147456,readback.data(),1));
 std::atomic<bool> copy_ok{true};
 std::thread reader([&] {
  std::array<unsigned char,8192> bytes{};
  for(int i=0;i<1000;++i)if(!memory.TryRead(address,bytes.data(),bytes.size()) || bytes!=source)copy_ok=false;
 });
 const bool committed=memory.Commit(196608,4096);
 reader.join();CHECK(committed && copy_ok);
 CHECK(memory.TryRead(base+196608,readback.data(),1));
 SharedDirectMemory rejected;CHECK(!rejected.InitializeFileBacked(capacity,L"C:\\"));
 std::printf("mode=%s\n",file_mode?"D-file":"pagefile");
 std::puts("PASS: actual shared section commits, NOACCESS/READONLY guest protection preserved, exact backing read/write, uncommitted and overflow rejection");
}