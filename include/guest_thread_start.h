#pragma once
#include <windows.h>

// The guest may read its own pthread handle in its first instruction.
// Publish it before allowing the entry point to run.
inline HANDLE CreatePublishedGuestThread(LPTHREAD_START_ROUTINE entry, void* arg,
                                        SIZE_T stack_reserve, void** guest_handle) {
    if (!entry || !guest_handle) return nullptr;
    HANDLE thread = CreateThread(nullptr, stack_reserve, entry, arg,
        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    *guest_handle = thread;
    if (!thread) return nullptr;
    if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
        *guest_handle = nullptr;
        // The entry point has never run; reclaim only this suspended thread.
        TerminateThread(thread, ERROR_OPERATION_ABORTED);
        CloseHandle(thread);
        return nullptr;
    }
    return thread;
}
