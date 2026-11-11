#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>

class MspaceArena {
public:
    MspaceArena(uint64_t base, uint64_t capacity) {
        if (base != 0 && capacity != 0 && capacity <= (std::numeric_limits<uint64_t>::max)() - base) {
            AddFreeRange(base, base + capacity);
        }
    }

    [[nodiscard]] uint64_t Allocate(uint64_t bytes, uint64_t alignment) {
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return 0;
        }
        for (auto it = m_free_by_size.lower_bound({bytes, 0}); it != m_free_by_size.end(); ++it) {
            const auto [length, begin] = *it;
            const uint64_t padding = (0 - begin) & (alignment - 1);
            if (padding > length - bytes) {
                continue;
            }
            const uint64_t address = begin + padding;
            const uint64_t after = address + bytes;
            m_live.emplace(address, bytes);
            const uint64_t end = begin + length;
            RemoveFreeRange(m_free.find(begin));
            if (begin < address) {
                AddFreeRange(begin, address);
            }
            if (after < end) {
                AddFreeRange(after, end);
            }
            m_used += bytes;
            m_peak = (std::max)(m_peak, m_used);
            return address;
        }
        return 0;
    }

    [[nodiscard]] bool Release(uint64_t address) {
        const auto allocation = m_live.find(address);
        if (allocation == m_live.end()) {
            return false;
        }

        uint64_t begin = address;
        uint64_t end = address + allocation->second;
        auto next = m_free.lower_bound(address);
        if (next != m_free.end() && next->first == end) {
            end = next->second;
            next = RemoveFreeRange(next);
        }
        if (next != m_free.begin()) {
            const auto previous = std::prev(next);
            if (previous->second == address) {
                begin = previous->first;
                RemoveFreeRange(previous);
            }
        }
        AddFreeRange(begin, end);
        m_used -= allocation->second;
        m_live.erase(allocation);
        return true;
    }

    [[nodiscard]] uint64_t Used() const { return m_used; }
    [[nodiscard]] uint64_t Peak() const { return m_peak; }

private:
    using FreeRanges = std::map<uint64_t, uint64_t>;

    void AddFreeRange(uint64_t begin, uint64_t end) {
        m_free.emplace(begin, end);
        m_free_by_size.emplace(end - begin, begin);
    }

    FreeRanges::iterator RemoveFreeRange(FreeRanges::iterator range) {
        const auto [begin, end] = *range;
        m_free_by_size.erase({end - begin, begin});
        return m_free.erase(range);
    }

    // Address order supports coalescing. Size order skips short fragments during allocation.
    FreeRanges m_free;
    std::map<uint64_t, uint64_t> m_live;
    uint64_t m_used = 0;
    uint64_t m_peak = 0;
    std::set<std::pair<uint64_t, uint64_t>> m_free_by_size;
};