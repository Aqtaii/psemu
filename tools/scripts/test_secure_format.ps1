$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'src\core.cpp'))
$first = $source.IndexOf('static bool SafeReadable(')
$last = $source.IndexOf('// printf/fprintf gibi DOGRUDAN', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Production secure formatting not found' }
$memoryEnd=$source.IndexOf('static int CallGuestHdrToneMapLuminance(', $first)
$stringStart=$source.IndexOf('static size_t SafeStrlen(', $memoryEnd)
if($memoryEnd -le $first -or $stringStart -le $memoryEnd){throw 'Memory/formatter production boundaries missing'}
$production=$source.Substring($first,$memoryEnd-$first)+$source.Substring($stringStart,$last-$stringStart)
$prefix = @"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <string>
#include <initializer_list>
std::atomic<uint64_t> g_vq_calls{0};
"@
$tests = @"
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
struct Args { uint32_t gp; uint32_t fp; void* overflow; void* registers; };
uint64_t Hash(const char* p) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*p) { h ^= static_cast<unsigned char>(*p++); h *= 0x100000001b3ULL; }
    return h;
}
int main() {
    alignas(16) uint8_t registers[176]{};
    uint64_t overflow[2]{};
    Args args{16,48,overflow,registers};
    char names[2][32]{};
    for (uint64_t i = 0; i < 2; ++i) {
        memcpy(registers + 16, &i, 8);
        CHECK(GuestVsprintfSecure(names[i],32,"__UNKNOWN__%d",reinterpret_cast<uint8_t*>(&args)) == 12);
    }
    CHECK(strcmp(names[0],"__UNKNOWN__0") == 0);
    CHECK(strcmp(names[1],"__UNKNOWN__1") == 0);
    CHECK(Hash(names[0]) != Hash(names[1]));
    CHECK(args.gp == 16 && args.fp == 48);
    const char* name = "depth"; uint64_t index = 3; double value = 1.5;
    memcpy(registers + 16, &name, 8); memcpy(registers + 24, &index, 8);
    memcpy(registers + 48, &value, 8);
    char mixed[32]{};
    CHECK(GuestVsprintfSecure(mixed,sizeof(mixed),"%s-%d-%.1f",reinterpret_cast<uint8_t*>(&args)) == 11);
    CHECK(strcmp(mixed,"depth-3-1.5") == 0);
    args.gp = 48; overflow[0] = 17;
    CHECK(GuestVsprintfSecure(mixed,sizeof(mixed),"__UNKNOWN__%d",reinterpret_cast<uint8_t*>(&args)) == 13);
    CHECK(strcmp(mixed,"__UNKNOWN__17") == 0);
    struct { char before; char buffer[4]; char after; } guarded{'L',{'x','x','x','x'},'R'};
    CHECK(GuestVsprintfSecure(guarded.buffer,4,"123",reinterpret_cast<uint8_t*>(&args)) == 3);
    CHECK(strcmp(guarded.buffer,"123") == 0 && guarded.before == 'L' && guarded.after == 'R');
    CHECK(GuestVsprintfSecure(guarded.buffer,4,"1234",reinterpret_cast<uint8_t*>(&args)) == -1);
    CHECK(guarded.buffer[0] == 0 && guarded.before == 'L' && guarded.after == 'R');
    guarded.buffer[0] = 'Z';
    CHECK(GuestVsprintfSecure(guarded.buffer,0,"a",reinterpret_cast<uint8_t*>(&args)) == -1 && guarded.buffer[0] == 'Z');
    CHECK(GuestVsprintfSecure(nullptr,4,"a",reinterpret_cast<uint8_t*>(&args)) == -1);
    CHECK(GuestVsprintfSecure(guarded.buffer,4,nullptr,reinterpret_cast<uint8_t*>(&args)) == -1 && guarded.buffer[0] == 0);
    CHECK(GuestVsprintfSecure(guarded.buffer,4,"a",nullptr) == -1);
    auto* page = static_cast<char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    CHECK(page != nullptr); page[0] = 'P'; DWORD old;
    CHECK(VirtualProtect(page,4096,PAGE_READONLY,&old));
    CHECK(GuestVsprintfSecure(page,4096,"depth",reinterpret_cast<uint8_t*>(&args)) == -1 && page[0] == 'P');
    VirtualFree(page,0,MEM_RELEASE);
    auto check_format=[](const char* format,const char* expected,std::initializer_list<uint64_t> values,double fp=1.5) {
        alignas(16) uint8_t regs[176]{};uint64_t stack[8]{};size_t index=0;
        for(auto value:values){if(index<4)memcpy(regs+16+index*8,&value,8);else stack[index-4]=value;++index;}
        memcpy(regs+48,&fp,8);Args va{16,48,stack,regs};char output[1024]{};
        const int n=GuestVsprintfSecure(output,sizeof(output),format,reinterpret_cast<uint8_t*>(&va));
        if(n!=static_cast<int>(strlen(expected)) || strcmp(output,expected)){std::printf("format=%s got=%s expected=%s\n",format,output,expected);return false;}
        return true;
    };
    CHECK(check_format("__UNKNOWN__%d","__UNKNOWN__0",{0xfeedface00000000ULL}));
    CHECK(check_format("%08x","80431068",{0xfeedface80431068ULL}));
    CHECK(check_format("%d","-2147483648",{0xfeedface80000000ULL}));
    CHECK(check_format("%u","4294967295",{0xfeedfaceffffffffULL}));
    CHECK(check_format("%hhd/%hd","-2/-2",{0xfeedfacefffffffeULL,0xfeedfacefffffffeULL}));
    CHECK(check_format("%hhu/%hu","171/43981",{0xfeedfaceffffffabULL,0xfeedfaceffffabcdULL}));
    CHECK(check_format("%ld/%lld/%zu/%td","4294967296/-2/4294967296/-2",{0x100000000ULL,~1ULL,0x100000000ULL,~1ULL}));
    CHECK(check_format("%jx","123456789abcdef0",{0x123456789abcdef0ULL}));
    CHECK(check_format("%0*x","80431068",{0xfeedface00000008ULL,0xfeedface80431068ULL}));
    CHECK(check_format("%*d","-2   ",{0xfeedfacefffffffbULL,0xfeedfacefffffffeULL}));
    CHECK(check_format("%*.*f","    1.50",{8,2}));
    CHECK(check_format("%*.*s","     abc",{8,3,reinterpret_cast<uint64_t>("abcdef")}));
    CHECK(check_format("%u/%u/%u/%u/%u","1/2/3/4/4294967295",{1,2,3,4,0xfeedfaceffffffffULL}));
    CHECK(check_format("%p","0x123456789abcdef0",{0x123456789abcdef0ULL}));
    std::string long_text(2048,'A');char long_buffer[2100]{};args.gp=16;const char* long_ptr=long_text.c_str();memcpy(registers+16,&long_ptr,8);
    CHECK(GuestVsprintfSecure(long_buffer,sizeof(long_buffer),"%s",reinterpret_cast<uint8_t*>(&args))==2048 && long_text==long_buffer);
    std::puts("PASS: resource identities, poisoned upper GP bits, LP64/narrow integers, star width/precision, FP/stack arguments, long strings and bounded output");
}
"@
$out = Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cpp = Join-Path $out 'secure_format_test.cpp'
$exe = Join-Path $out 'secure_format_test.exe'
[IO.File]::WriteAllText($cpp, "$prefix`n$production`n$tests", [Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /Od /EHsc /MD $cpp "/Fe:$exe" "/Fo:$out\secure_format_test.obj"
if ($LASTEXITCODE -ne 0) { throw 'Secure formatting compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Secure formatting regression failed' }
