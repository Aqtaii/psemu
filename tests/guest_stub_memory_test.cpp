#include "guest_stub_memory.h"
#include <cstdio>
#include <cstring>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    CHECK(!TryInstallGuestReturnZero(0));
    CHECK(!TryInstallGuestReturnZero(1));
    CHECK(!TryInstallGuestReturnZero(0xffff));
    CHECK(!TryInstallGuestReturnZero((std::numeric_limits<uint64_t>::max)()));
    CHECK(!TryInstallGuestReturnZero((std::numeric_limits<uint64_t>::max)() - 1));
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto* reserved = static_cast<uint8_t*>(VirtualAlloc(nullptr,system.dwAllocationGranularity,MEM_RESERVE,PAGE_NOACCESS));
    CHECK(reserved != nullptr);
    // The stub starts at the last byte of an uncommitted page: both pages must be committed.
    const uint64_t boundary = reinterpret_cast<uint64_t>(reserved) + system.dwPageSize - 1;
    CHECK(TryInstallGuestReturnZero(boundary));
    auto* code = reinterpret_cast<const uint8_t*>(boundary);
    CHECK(code[0] == 0x31 && code[1] == 0xc0 && code[2] == 0xc3);
    CHECK(reinterpret_cast<uint64_t(*)()>(boundary)() == 0);
    VirtualFree(reserved,0,MEM_RELEASE);
    auto* readOnly = static_cast<uint8_t*>(VirtualAlloc(nullptr,system.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(readOnly != nullptr); memset(readOnly,0x5a,system.dwPageSize); DWORD old;
    CHECK(VirtualProtect(readOnly,system.dwPageSize,PAGE_READONLY,&old));
    CHECK(TryInstallGuestReturnZero(reinterpret_cast<uint64_t>(readOnly) + 16));
    CHECK(readOnly[15] == 0x5a && readOnly[19] == 0x5a);
    CHECK(reinterpret_cast<uint64_t(*)()>(readOnly + 16)() == 0);
    VirtualFree(readOnly,0,MEM_RELEASE);
    // A reservation at a non-granularity-aligned free address must not be accepted at its rounded base.
    void* probe = VirtualAlloc(nullptr,system.dwAllocationGranularity,MEM_RESERVE,PAGE_NOACCESS);
    CHECK(probe != nullptr); const uint64_t freeBase = reinterpret_cast<uint64_t>(probe);
    VirtualFree(probe,0,MEM_RELEASE);
    CHECK(!TryInstallGuestReturnZero(freeBase + system.dwPageSize));
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(reinterpret_cast<void*>(freeBase),&info,sizeof(info)) != 0 && info.State == MEM_FREE);
    CHECK(TryInstallGuestReturnZero(freeBase));
    CHECK(reinterpret_cast<uint64_t(*)()>(freeBase)() == 0);
    VirtualFree(reinterpret_cast<void*>(freeBase),0,MEM_RELEASE);
    std::puts("PASS: null/low/overflow rejection, two-page commitment, exact reservation and executable fallback");
}
