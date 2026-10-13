#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
// Error contracts and bounded path components follow Jetsku/KytyPS5
// f9e19582 src/libs/libSaveData.cpp (GPL-2.0).
inline bool GuestSaveComponent(std::string_view value, std::size_t capacity) {
    if (value.empty() || value.size() >= capacity || value == "." || value == "..") return false;
    for (unsigned char c : value) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == 127) return false;
    }
    return true;
}
inline int32_t GuestSaveTransferStatus(int32_t user, std::string_view title,
                                      std::string_view directory, bool archive_exists) {
    if (!GuestSaveComponent(title,10) || !GuestSaveComponent(directory,32))
        return static_cast<int32_t>(0x809f0000u);
    if (user < 0) return static_cast<int32_t>(0x809f0011u);
    // Transfer does not create an archive. Existing cross-title imports are
    // unsupported by this VFS backend and must fail instead of returning an empty success.
    return static_cast<int32_t>(archive_exists ? 0x809f0005u : 0x809f0008u);
}
