#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <winioctl.h>

// Separate views share committed pages, while guest watch protections stay local.
class SharedDirectMemory {
public:
    ~SharedDirectMemory() {
        if (m_guest) UnmapViewOfFile(m_guest);
        if (m_backing) UnmapViewOfFile(m_backing);
        if (m_section) CloseHandle(m_section);
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
    }
    SharedDirectMemory() = default;
    SharedDirectMemory(const SharedDirectMemory&) = delete;
    SharedDirectMemory& operator=(const SharedDirectMemory&) = delete;
    bool Initialize(uint64_t size) {
        if (m_section || !size) return false;
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_EXECUTE_READWRITE | SEC_RESERVE, static_cast<DWORD>(size >> 32),
            static_cast<DWORD>(size), nullptr);
        if (!section) return false;
        auto guest = static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, static_cast<SIZE_T>(size)));
        auto backing = static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size)));
        if (!guest || !backing) {
            if (guest) UnmapViewOfFile(guest);
            if (backing) UnmapViewOfFile(backing);
            CloseHandle(section);
            return false;
        }
        m_section = section; m_guest = guest; m_backing = backing; m_size = size;
        return true;
    }
    // Optional scratch-file backing avoids charging a full physical pool to the
    // Windows pagefile. CREATE_NEW + DELETE_ON_CLOSE never overwrites user data.
    // Unallocated pages remain inaccessible in both views, exactly as SEC_RESERVE.
    bool InitializeFileBacked(uint64_t size, const wchar_t* directory) {
        if (m_section || m_file != INVALID_HANDLE_VALUE || !size || !directory) return false;
        wchar_t folder[MAX_PATH]{};
        const auto length = GetFullPathNameW(directory, MAX_PATH, folder, nullptr);
        if (!length || length >= MAX_PATH || (folder[0] != L'D' && folder[0] != L'd') ||
            folder[1] != L':' || folder[2] != L'\\') return false;
        wchar_t path[MAX_PATH]{};
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            if (swprintf_s(path, L"%s\\psemu-dmem-%lu-%llu-%u.tmp", folder,
                           GetCurrentProcessId(), GetTickCount64(), attempt) < 0) return false;
            m_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (m_file != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_FILE_EXISTS) return false;
        }
        if (m_file == INVALID_HANDLE_VALUE) return false;
        DWORD bytes = 0;
        LARGE_INTEGER end{}; end.QuadPart = static_cast<LONGLONG>(size);
        if (!DeviceIoControl(m_file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr) ||
            !SetFilePointerEx(m_file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(m_file)) {
            ResetFile(); return false;
        }
        m_section = CreateFileMappingW(m_file, nullptr, PAGE_EXECUTE_READWRITE,
            static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), nullptr);
        if (!m_section) { ResetFile(); return false; }
        m_guest = static_cast<uint8_t*>(MapViewOfFile(m_section, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, size));
        m_backing = static_cast<uint8_t*>(MapViewOfFile(m_section, FILE_MAP_ALL_ACCESS, 0, 0, size));
        DWORD old = 0;
        if (!m_guest || !m_backing || !VirtualProtect(m_guest, size, PAGE_NOACCESS, &old) ||
            !VirtualProtect(m_backing, size, PAGE_NOACCESS, &old)) {
            if (m_guest) UnmapViewOfFile(m_guest);
            if (m_backing) UnmapViewOfFile(m_backing);
            CloseHandle(m_section); m_section = nullptr;
            m_guest = m_backing = nullptr; ResetFile(); return false;
        }
        m_size = size;
        return true;
    }
    bool Commit(uint64_t offset, uint64_t size) {
        if (!m_guest || !size || offset >= m_size || size > m_size - offset) return false;
        // The writable alias's guards are the allocation map; GPU guest guards
        // are independent. Recommit only enables new pages and preserves old guards.
        std::lock_guard lock(m_commit_mutex);
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        const uint64_t mask = system.dwPageSize - 1;
        uint64_t current = offset & ~mask;
        const uint64_t end = (offset + size + mask) & ~mask;
        if (end > m_size) return false;
        while (current < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(m_backing + current, &info, sizeof(info))) return false;
            const auto region_end = static_cast<uint64_t>(static_cast<uint8_t*>(info.BaseAddress) - m_backing) + info.RegionSize;
            const auto next = end < region_end ? end : region_end;
            if (next <= current) return false;
            if (m_file == INVALID_HANDLE_VALUE && info.State == MEM_RESERVE) {
                if (!VirtualAlloc(m_guest + current, next - current, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return false;
            } else if (m_file != INVALID_HANDLE_VALUE && info.Protect == PAGE_NOACCESS) {
                DWORD old = 0;
                if (!VirtualProtect(m_guest + current, next - current, PAGE_EXECUTE_READWRITE, &old)) return false;
                if (!VirtualProtect(m_backing + current, next - current, PAGE_READWRITE, &old)) {
                    VirtualProtect(m_guest + current, next - current, PAGE_NOACCESS, &old);
                    return false;
                }
            }
            current = next;
        }
        return true;
    }
    uint8_t* GuestBase() const { return m_guest; }
    bool TryRead(uint64_t address, void* data, uint64_t size) const {
        auto p = Translate(address, size);
        if (!p || !data || !Readable(p, size)) return false;
        std::memcpy(data, p, static_cast<size_t>(size)); return true;
    }
    bool TryWrite(uint64_t address, const void* data, uint64_t size) const {
        auto p = Translate(address, size);
        if (!p || !data || !Readable(p, size)) return false;
        std::memcpy(p, data, static_cast<size_t>(size)); return true;
    }
private:
    void ResetFile() { CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE; }
    uint8_t* Translate(uint64_t address, uint64_t size) const {
        const auto base = reinterpret_cast<uint64_t>(m_guest);
        if (!m_guest || !size || address < base) return nullptr;
        const auto offset = address - base;
        if (offset >= m_size || size > m_size - offset) return nullptr;
        return m_backing + offset;
    }
    static bool Readable(uint8_t* p, uint64_t size) {
        auto address = reinterpret_cast<uintptr_t>(p);
        const auto end = address + size;
        while (address < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return false;
            const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (next <= address) return false;
            address = next;
        }
        return true;
    }
    HANDLE m_file = INVALID_HANDLE_VALUE;
    std::mutex m_commit_mutex;
    HANDLE m_section = nullptr;
    uint8_t* m_guest = nullptr;
    uint8_t* m_backing = nullptr;
    uint64_t m_size = 0;
};
