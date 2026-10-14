#pragma once
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <limits>
inline constexpr std::int64_t GuestReadBadFd = -2147352567;
inline constexpr std::int64_t GuestReadFault = -2147352562;
inline constexpr std::int64_t GuestReadInvalid = -2147352554;
inline constexpr std::int64_t GuestReadIoError = -2147352571;
// Caller holds the VFS lock across seek/read/restore and validates the guest span.
inline std::int64_t GuestPositionedRead(FILE* file, void* buffer, std::size_t bytes,
                                       std::int64_t offset) {
    if (!file) return GuestReadBadFd;
    if (offset < 0 || bytes > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()))
        return GuestReadInvalid;
    if (bytes == 0) return 0;
    if (!buffer) return GuestReadFault;
    const auto saved = _ftelli64(file);
    if (saved < 0) return GuestReadIoError;
    if (_fseeki64(file, offset, SEEK_SET) != 0) return GuestReadIoError;
    const auto read = std::fread(buffer, 1, bytes, file);
    const bool failed = std::ferror(file) != 0;
    if (_fseeki64(file, saved, SEEK_SET) != 0) return GuestReadIoError;
    if (failed && read == 0) return GuestReadIoError;
    return static_cast<std::int64_t>(read);
}
