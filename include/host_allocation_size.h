#pragma once
#include <cstddef>
#include <cstdint>
// Zero means overflow; callers must not pass it to the CRT allocator.
inline size_t GuestHostAllocationSize(size_t requested, bool legacy_padding) {
    if (!legacy_padding) return requested < 16 ? 16 : requested;
    if (requested > SIZE_MAX - 65536) return 0;
    return requested + 65536;
}
