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

inline int GuestStrcpyS(char* destination, std::size_t capacity, std::string_view source) {
    return GuestStrncpyS(destination,capacity,source,source.size());
}
inline int GuestWriteFormattedString(char* destination, std::size_t capacity, std::string_view text) {
    if (!destination || capacity == 0) return -1;
    if (text.size() >= capacity || text.size() > 0x7fffffffULL) {
        destination[0]=0;
        return -1;
    }
    if (!text.empty()) std::memcpy(destination,text.data(),text.size());
    destination[text.size()]=0;
    return static_cast<int>(text.size());
}

inline int GuestStrncatS(char* destination, std::size_t capacity,
                         std::string_view source, std::size_t count) {
    if (!destination || capacity == 0) return 22;
    std::size_t length = 0;
    while (length < capacity && destination[length]) ++length;
    if (length == capacity) { destination[0] = 0; return 22; }
    const bool truncate = count == (std::numeric_limits<std::size_t>::max)();
    const auto requested = truncate ? source.size() : (std::min)(source.size(), count);
    const auto available = capacity - length - 1;
    if (!truncate && requested > available) { destination[0] = 0; return 34; }
    const auto copied = (std::min)(requested, available);
    if (copied) std::memcpy(destination + length, source.data(), copied);
    destination[length + copied] = 0;
    return truncate && requested > available ? 80 : 0;
}
