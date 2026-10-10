#pragma once
#include <cstdint>
#include <cstddef>
#include <sys/stat.h>
// PS5 layout reference: KytyPS5 src/kernel/fileSystem.h, GPL-2.0.
// https://github.com/KytyPS5/KytyPS5/blob/307fdcf7ad5d9d15b5966288fcc598ac9dbca188/src/kernel/fileSystem.h
struct GuestFileTime { int64_t seconds; int64_t nanoseconds; };
struct GuestFileStat {
    uint32_t device, inode;
    uint16_t mode, links;
    uint32_t uid, gid, rdevice;
    GuestFileTime accessed, modified, changed;
    int64_t size, blocks;
    uint32_t block_size, flags, generation;
    int32_t spare;
    GuestFileTime born;
};
static_assert(sizeof(GuestFileStat) == 120);
static_assert(offsetof(GuestFileStat, size) == 72);
static_assert(offsetof(GuestFileStat, born) == 104);
inline GuestFileStat MakeGuestFileStat(const struct _stat64& host) {
    GuestFileStat result{};
    result.device = static_cast<uint32_t>(host.st_dev);
    result.inode = static_cast<uint32_t>(host.st_ino);
    result.mode = static_cast<uint16_t>(0777 | ((host.st_mode & _S_IFDIR) ? 0040000 : 0100000));
    result.links = static_cast<uint16_t>(host.st_nlink);
    result.size = host.st_size;
    result.blocks = (host.st_size + 511) / 512;
    result.block_size = 512;
    result.accessed.seconds = host.st_atime;
    result.modified.seconds = host.st_mtime;
    result.changed.seconds = host.st_ctime;
    result.born = result.changed;
    return result;
}
