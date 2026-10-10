#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

// Separate views share committed pages, while guest watch protections stay local.
class SharedDirectMemory {
public:
    ~SharedDirectMemory() {
        if (m_guest) UnmapViewOfFile(m_guest);
        if (m_backing) UnmapViewOfFile(m_backing);
        if (m_section) CloseHandle(m_section);
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
    HANDLE m_section = nullptr;
    uint8_t* m_guest = nullptr;
    uint8_t* m_backing = nullptr;
    uint64_t m_size = 0;
};
