#pragma once
#include <windows.h>
#include <cstdint>

// Native atomic imports execute under a local SEH boundary. The emulator VEH
// must defer these faults instead of auto-committing or changing guest pages.
namespace NativeAtomicGuard {
extern thread_local bool active;
inline bool DeferException(DWORD code) {
    return active && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_GUARD_PAGE);
}
enum class Operation { Add, Subtract, Load, CompareExchange };

int32_t Perform(Operation operation, volatile const int32_t* obj,
                int32_t* expected, int32_t value);
} // namespace NativeAtomicGuard
