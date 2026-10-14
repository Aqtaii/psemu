#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
inline int GuestMemcpyS(void* destination, std::size_t capacity,
                        const void* source, std::size_t count) {
    if (count==0) return 0;
    if (!destination) return 22;
    if (!source || count>capacity) {
        if (capacity) std::memset(destination,0,capacity);
        return !source ? 22 : 34;
    }
    const auto da=reinterpret_cast<std::uintptr_t>(destination);
    const auto sa=reinterpret_cast<std::uintptr_t>(source);
    if (da>=sa ? da-sa<count : sa-da<count) {
        if (capacity) std::memset(destination,0,capacity);
        return 22;
    }
    std::memcpy(destination,source,count);
    return 0;
}

inline int GuestMemmoveS(void* destination, std::size_t capacity,
                         const void* source, std::size_t count) {
    if (count==0) return 0;
    if (!destination || !source) return 22;
    if (count>capacity) return 34;
    std::memmove(destination,source,count);
    return 0;
}
