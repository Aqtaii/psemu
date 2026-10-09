#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace GuestDetour {

// E8 stores a SIGNED 32-bit displacement from the end of its five bytes.
inline bool RelativeCallOffset(uint64_t site, uint64_t target, int32_t& offset) {
    if (site > UINT64_MAX - 5) return false;
    const uint64_t next = site + 5;
    if (target >= next) {
        const uint64_t distance = target - next;
        if (distance > INT32_MAX) return false;
        offset = static_cast<int32_t>(distance);
    } else {
        const uint64_t distance = next - target;
        if (distance > uint64_t(INT32_MAX) + 1) return false;
        offset = distance == uint64_t(INT32_MAX) + 1 ? INT32_MIN
                                                     : -static_cast<int32_t>(distance);
    }
    return true;
}

inline uint8_t* AllocateNear(const void* site, size_t size) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t address = reinterpret_cast<uintptr_t>(site);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t minimum = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    // Leave room for nearby call sites and rounding at the signed-rel32 edges.
    const uintptr_t reach = 0x70000000;
    if (address < minimum || address > maximum || size == 0 || size > granularity) return nullptr;
    const uintptr_t low = address - minimum > reach ? address - reach : minimum;
    const uintptr_t high = maximum - address > reach ? address + reach : maximum;
    auto scan = [&](uintptr_t begin, uintptr_t end) -> uint8_t* {
        uintptr_t candidate = (begin + granularity - 1) & ~(granularity - 1);
        while (candidate < end && size <= end - candidate) {
            MEMORY_BASIC_INFORMATION region;
            if (VirtualQuery(reinterpret_cast<void*>(candidate), &region, sizeof(region)) == 0) break;
            const uintptr_t region_end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (region_end <= candidate) break;
            if (region.State == MEM_FREE && size <= region_end - candidate) {
                if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                    return static_cast<uint8_t*>(memory);
                }
                candidate += granularity;
            } else {
                candidate = (region_end + granularity - 1) & ~(granularity - 1);
            }
        }
        return nullptr;
    };
    if (auto* memory = scan(address, high)) return memory;
    return scan(low, address);
}

inline bool InstallCall(uint8_t* site, const void* target, const void* original_target) {
    int32_t replacement = 0, original = 0;
    if (!site || !RelativeCallOffset(reinterpret_cast<uint64_t>(site),
                                     reinterpret_cast<uint64_t>(target), replacement) ||
        !RelativeCallOffset(reinterpret_cast<uint64_t>(site),
                            reinterpret_cast<uint64_t>(original_target), original)) return false;
    int32_t current;
    std::memcpy(&current, site + 1, sizeof(current));
    if (site[0] != 0xe8 || current != original) return false;
    DWORD previous;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(site + 1, &replacement, sizeof(replacement));
    DWORD ignored;
    VirtualProtect(site, 5, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

} // namespace GuestDetour
