#include "guest_thread_start.h"
#include <cassert>
#include <cstdio>
struct State { void* handle = nullptr; DWORD observed_id = 0; };
static DWORD WINAPI Entry(void* arg) {
    auto* state = static_cast<State*>(arg);
    // Models Astro: inspect the published handle immediately upon entry.
    state->observed_id = GetThreadId(static_cast<HANDLE>(state->handle));
    return state->observed_id == GetCurrentThreadId() ? 0 : 1;
}
int main() {
    for (int i = 0; i < 512; ++i) {
        State state;
        HANDLE h = CreatePublishedGuestThread(Entry, &state, 1024*1024, &state.handle);
        assert(h && state.handle == h);
        assert(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
        DWORD result = 1;
        assert(GetExitCodeThread(h, &result) && result == 0);
        assert(state.observed_id != 0);
        CloseHandle(h);
    }
    State invalid;
    assert(!CreatePublishedGuestThread(nullptr, &invalid, 1024*1024, &invalid.handle));
    assert(!CreatePublishedGuestThread(Entry, &invalid, 1024*1024, nullptr));
    puts("Guest thread handle publication: 512 immediate-entry runs passed");
}
