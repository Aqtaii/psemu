#include "native_atomic_guard.h"
#include <intrin.h>

namespace NativeAtomicGuard {
thread_local bool active = false;

static int Filter(EXCEPTION_POINTERS* info) {
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

// Hardware-fault recovery needs clang-cl /EHa. Keep it in this Windows ABI
// translation unit; the guest imports and emulator core retain their flags.
__declspec(noinline) int32_t Perform(Operation operation, volatile const int32_t* obj,
                                           int32_t* expected, int32_t value) {
    const bool previous = active;
    active = true;
    int32_t result = 0;
    __try {
        auto* target = reinterpret_cast<volatile long*>(const_cast<int32_t*>(obj));
        switch (operation) {
        case Operation::Add:
            result = _InterlockedExchangeAdd(target, value);
            break;
        case Operation::Subtract:
            result = _InterlockedExchangeAdd(target, static_cast<long>(0u - static_cast<uint32_t>(value)));
            break;
        case Operation::Load:
            result = *obj;
            break;
        case Operation::CompareExchange: {
            // Check write access before changing obj; the probe preserves expected.
            const long exp = _InterlockedCompareExchange(reinterpret_cast<volatile long*>(expected), 0, 0);
            const long old = _InterlockedCompareExchange(target, value, exp);
            if (old == exp) result = 1;
            else *expected = old;
            break;
        }
        }
    } __except (Filter(GetExceptionInformation())) {}
    active = previous;
    return result;
}
} // namespace NativeAtomicGuard
