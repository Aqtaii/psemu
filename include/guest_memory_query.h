#pragma once
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstdint>

// VirtualQuery may walk gigabytes of contiguous mapped-file pages just to find
// RegionSize. Resident pages expose current protection directly; return only
// that page's extent, never infer or cache neighbouring page permissions.
// Invalid working-set entries carry no usable protection: fall back unchanged.
namespace GuestMemoryQuery {
inline SIZE_T Query(const void* address, MEMORY_BASIC_INFORMATION* info,
                    std::atomic<uint64_t>* fallback_queries = nullptr) {
    PSAPI_WORKING_SET_EX_INFORMATION page{};
    page.VirtualAddress = const_cast<void*>(address);
    if (K32QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page)) &&
        page.VirtualAttributes.Valid) {
        static const SIZE_T page_size = [] {
            SYSTEM_INFO system{}; GetSystemInfo(&system); return SIZE_T(system.dwPageSize);
        }();
        const auto base = reinterpret_cast<uintptr_t>(address) & ~(uintptr_t(page_size) - 1);
        *info = {};
        info->BaseAddress = reinterpret_cast<void*>(base);
        info->RegionSize = page_size;
        info->State = MEM_COMMIT;
        info->Protect = static_cast<DWORD>(page.VirtualAttributes.Win32Protection);
        return sizeof(*info);
    }
    if (fallback_queries) fallback_queries->fetch_add(1, std::memory_order_relaxed);
    return VirtualQuery(address, info, sizeof(*info));
}
} // namespace GuestMemoryQuery
