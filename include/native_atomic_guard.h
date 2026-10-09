#pragma once
#include <windows.h>
#include <cstdint>

// Native atomic imports execute under a local SEH boundary. The emulator VEH
// must defer these faults instead of auto-committing or changing guest pages.
namespace NativeAtomicGuard {
inline thread_local bool active = false;
inline bool DeferException(DWORD code) {
    return active && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_GUARD_PAGE);
}
inline int Filter(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_GUARD_PAGE)
        return EXCEPTION_CONTINUE_SEARCH;
    // Windows clears PAGE_GUARD when accessed. Preserve the validation behavior
    // of the former VirtualQuery path: rejected atomics leave guard pages armed.
    if (code == EXCEPTION_GUARD_PAGE && info->ExceptionRecord->NumberParameters >= 2) {
        const auto address = info->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT) {
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            const auto page = address & ~(uintptr_t(system.dwPageSize) - 1);
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void*>(page), system.dwPageSize,
                           mbi.Protect | PAGE_GUARD, &old);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace NativeAtomicGuard
