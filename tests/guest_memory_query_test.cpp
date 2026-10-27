#define NOMINMAX
#include "guest_memory_query.h"
#include <cstdio>
#include <chrono>
#include <winioctl.h>
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s error=%lu\n",__LINE__,#x,GetLastError());return 1;}}while(0)
int main() {
 SYSTEM_INFO si{};GetSystemInfo(&si);const size_t page=si.dwPageSize;
 std::atomic<uint64_t> fallbacks{0};MEMORY_BASIC_INFORMATION info{};
 auto* mem=static_cast<char*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE,PAGE_NOACCESS));CHECK(mem);
 CHECK(GuestMemoryQuery::Query(mem,&info,&fallbacks) && info.State==MEM_RESERVE && fallbacks==1);
 CHECK(VirtualAlloc(mem,page*2,MEM_COMMIT,PAGE_READWRITE));
 CHECK(GuestMemoryQuery::Query(mem,&info,&fallbacks) && info.State==MEM_COMMIT && info.Protect==PAGE_READWRITE);
 mem[0]='a';CHECK(VirtualLock(mem,page));
 const auto before=fallbacks.load();
 CHECK(GuestMemoryQuery::Query(mem,&info,&fallbacks) && info.BaseAddress==mem && info.RegionSize==page && info.Protect==PAGE_READWRITE);
 CHECK(fallbacks==before);
 DWORD old=0;
 CHECK(VirtualProtect(mem,page,PAGE_READONLY,&old));
 CHECK(GuestMemoryQuery::Query(mem,&info) && info.Protect==PAGE_READONLY);
 CHECK(VirtualProtect(mem,page,PAGE_NOACCESS,&old));
 CHECK(GuestMemoryQuery::Query(mem,&info) && info.Protect==PAGE_NOACCESS);
 CHECK(VirtualProtect(mem,page,PAGE_READWRITE|PAGE_GUARD,&old));
 CHECK(GuestMemoryQuery::Query(mem,&info) && (info.Protect&PAGE_GUARD));
 MEMORY_BASIC_INFORMATION actual{};CHECK(VirtualQuery(mem,&actual,sizeof(actual)) && (actual.Protect&PAGE_GUARD));
 CHECK(VirtualProtect(mem,page,PAGE_READWRITE,&old));mem[0]='b';
 CHECK(GuestMemoryQuery::Query(mem,&info) && info.Protect==PAGE_READWRITE);
 VirtualUnlock(mem,page);CHECK(VirtualFree(mem,0,MEM_RELEASE));
 CHECK(GuestMemoryQuery::Query(mem,&info) && info.State==MEM_FREE);
 // A real sparse mapped file reproduces the expensive large-region query.
 wchar_t path[MAX_PATH]{};swprintf_s(path,L"D:\\Tools\\psemu-build\\temp\\query-test-%lu.tmp",GetCurrentProcessId());
 HANDLE file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
 DWORD returned=0;CHECK(DeviceIoControl(file,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr));
 LARGE_INTEGER size{};size.QuadPart=1ull<<30;CHECK(SetFilePointerEx(file,size,nullptr,FILE_BEGIN)&&SetEndOfFile(file));
 HANDLE section=CreateFileMappingW(file,nullptr,PAGE_READWRITE,0,0,nullptr);CHECK(section);
 auto* view=static_cast<char*>(MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,0));CHECK(view);
 auto* alias=static_cast<char*>(MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,0));CHECK(alias);
 CHECK(VirtualProtect(view,size.QuadPart,PAGE_NOACCESS,&old));
 CHECK(VirtualProtect(view,size.QuadPart,PAGE_READWRITE,&old));
 view[0]='x';CHECK(VirtualLock(view,page));
 const auto t0=std::chrono::steady_clock::now();CHECK(VirtualQuery(view,&actual,sizeof(actual)));
 const auto t1=std::chrono::steady_clock::now();
 const auto before_file=fallbacks.load();CHECK(GuestMemoryQuery::Query(view,&info,&fallbacks));
 const auto t2=std::chrono::steady_clock::now();
 CHECK(info.RegionSize==page && info.Protect==PAGE_READWRITE && fallbacks==before_file);
 CHECK(VirtualProtect(view,page,PAGE_NOACCESS,&old));
 CHECK(GuestMemoryQuery::Query(view,&info) && info.Protect==PAGE_NOACCESS);
 CHECK(GuestMemoryQuery::Query(alias,&info) && info.Protect==PAGE_READWRITE && alias[0]=='x');
 CHECK(VirtualProtect(view,page,PAGE_READWRITE,&old));VirtualUnlock(view,page);
 CHECK(UnmapViewOfFile(alias)&&UnmapViewOfFile(view)&&CloseHandle(section)&&CloseHandle(file));
 CHECK(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES);
 std::printf("[MEMORY-QUERY-TEST] current permissions, resident page, nonresident/reserved/free fallback, NOACCESS/GUARD, independent mapped aliases and D-file cleanup passed; large_region_ms=%.3f page_query_ms=%.3f game_frame=0\n",std::chrono::duration<double,std::milli>(t1-t0).count(),std::chrono::duration<double,std::milli>(t2-t1).count());
}

