
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    using Search = const char* (__attribute__((sysv_abi)) *)(const char*, int);
    Search first = &NativeStrchr, last = &NativeStrrchr;
    const char sample[] = "aa/bb/cc";
    for (int c : std::initializer_list<int>{0, '/', 'a', 'z', 0x100, 0x12f, -1}) {
        CHECK(first(sample,c)==std::strchr(sample,c));
        CHECK(last(sample,c)==std::strrchr(sample,c));
    }
    CHECK(first("",0)!=nullptr && last("",'x')==nullptr);
    CHECK(first(nullptr,0)==nullptr && last(reinterpret_cast<const char*>(1),'x')==nullptr);
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const size_t page=info.dwPageSize;
    auto* block=static_cast<char*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(block);
    std::memset(block,'a',page*4);
    block[page-2]='x'; block[page+5]='x'; block[page*2+1]=0;
    DWORD old=0;
    CHECK(VirtualProtect(block+page,page,PAGE_READONLY,&old));
    CHECK(first(block,'x')==block+page-2 && last(block,'x')==block+page+5);
    CHECK(first(block,0)==block+page*2+1 && last(block,0x100)==block+page*2+1);
    CHECK(VirtualProtect(block+page*3,page,PAGE_NOACCESS,&old));
    block[page*3-1]=0;
    CHECK(first(block+page*3-1,0)==block+page*3-1);
    CHECK(last(block+page*3-2,'a')==block+page*3-2);
    CHECK(first(block+page*3,'x')==nullptr);
    // Unterminated readable input must stop at NOACCESS, never fabricate a NUL.
    block[page*3-1]='a';
    CHECK(first(block+page*2,0)==block+page*2+1);
    block[page*2+1]='a';
    CHECK(first(block+page*2,0)==nullptr && last(block+page*2,0)==nullptr);
    CHECK(last(block+page*2,'a')==block+page*3-1);
    CHECK(VirtualProtect(block+page*3,page,PAGE_READWRITE,&old));
    CHECK(VirtualProtect(block+page*3,page,PAGE_READWRITE|PAGE_GUARD,&old));
    CHECK(first(block+page*3,'a')==nullptr);
    MEMORY_BASIC_INFORMATION mbi{};
    CHECK(VirtualQuery(block+page*3,&mbi,sizeof(mbi)) && (mbi.Protect&PAGE_GUARD));
    CHECK(VirtualFree(block,0,MEM_RELEASE));
    // Large XML-like suffix: early search does not allocate/copy/scan the suffix.
    std::string xml(2*1024*1024,'a'); xml[1]='>'; xml[xml.size()-2]='>';
    CHECK(first(xml.c_str(),'>')==xml.c_str()+1);
    CHECK(last(xml.c_str(),'>')==xml.c_str()+xml.size()-2);
    for (int i=0;i<10000;++i)CHECK(first(xml.c_str(),'>')==xml.c_str()+1);
    std::puts("[CHARACTER-SEARCH-TEST] SysV pointers, CRT semantics, byte truncation, region boundaries, readonly/NOACCESS/GUARD, unterminated input and large suffix passed");
}

