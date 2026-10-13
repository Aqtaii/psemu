#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
// strncpy_s contract: copied prefix always has a terminator; no zero padding.
// source contains the bounded readable prefix collected by the HLE handler.
inline int GuestStrncpyS(char* destination, std::size_t capacity,
                         std::string_view source, std::size_t count) {
    if (!destination || capacity == 0) return 22;
    const bool truncate = count == (std::numeric_limits<std::size_t>::max)();
    const auto copied = truncate ? (std::min)(source.size(),capacity-1)
                                 : (std::min)(source.size(),count);
    if (copied >= capacity) { destination[0]=0; return 34; }
    if (copied) std::memcpy(destination,source.data(),copied);
    destination[copied]=0;
    return truncate && source.size() >= capacity ? 80 : 0;
}
