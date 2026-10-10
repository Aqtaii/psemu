#pragma once
#include <windows.h>
#include <cstdint>

// Install a three-byte fallback only at the requested guest address.
inline bool TryInstallGuestReturnZero(uint64_t address) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto low = reinterpret_cast<uint64_t>(system.lpMinimumApplicationAddress);
    const auto high = reinterpret_cast<uint64_t>(system.lpMaximumApplicationAddress);
    if (address < low || address > high || high - address < 2) return false;
    const uint64_t pageSize = system.dwPageSize;
    const uint64_t start = address & ~(pageSize - 1);
    const uint64_t last = (address + 2) & ~(pageSize - 1);
    const SIZE_T span = static_cast<SIZE_T>(last - start + pageSize);
    void* page = reinterpret_cast<void*>(start);
    DWORD old = 0;
    bool ready = VirtualProtect(page, span, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (!ready) {
        void* committed = VirtualAlloc(page, span, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        ready = committed == page;
        if (!ready) {
            void* reserved = VirtualAlloc(page, span, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            ready = reserved == page;
            // Windows can round a reservation down to allocation granularity.
            // Such a reservation does not prove the requested span is committed.
            if (reserved != nullptr && !ready) VirtualFree(reserved, 0, MEM_RELEASE);
        }
    }
    if (!ready) return false;
    auto* code = reinterpret_cast<uint8_t*>(address);
    code[0] = 0x31; code[1] = 0xc0; code[2] = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), code, 3);
    return true;
}
