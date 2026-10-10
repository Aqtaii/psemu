#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <utility>

namespace Libs::Graphics {
// Serialized SSBO layout: four uint64 words per entry. Ranges, rather than a
// dense 48-bit page table, keep metadata proportional to resident cache owners.
struct alignas(32) GuestAddressRange {
 uint64_t guest_base = 0;
 uint64_t device_base = 0;
 uint64_t size = 0;
 uint64_t reserved = 0;
 bool operator==(const GuestAddressRange&) const = default;
};
static_assert(sizeof(GuestAddressRange) == 32);
static_assert(offsetof(GuestAddressRange, device_base) == 8);
static_assert(offsetof(GuestAddressRange, size) == 16);

inline constexpr uint64_t GuestAddressLimit = uint64_t{1} << 48;
inline constexpr size_t GuestAddressMaxRanges = 65536;

// Transactional: rejected input cannot replace an earlier command's snapshot.
inline bool BuildGuestAddressTable(std::span<const GuestAddressRange> ranges,
                                  std::vector<GuestAddressRange>* output) {
 if (!output || ranges.size() > GuestAddressMaxRanges) return false;
 std::vector<GuestAddressRange> sorted(ranges.begin(), ranges.end());
 std::sort(sorted.begin(), sorted.end(), [](const auto& a,const auto& b) {
  return a.guest_base < b.guest_base;
 });
 uint64_t end = 0;
 for (const auto& range: sorted) {
  if (!range.guest_base || range.guest_base >= GuestAddressLimit ||
      !range.size || range.size > GuestAddressLimit - range.guest_base ||
      !range.device_base || range.size - 1 > UINT64_MAX - range.device_base ||
      range.reserved || range.guest_base < end) return false;
  end = range.guest_base + range.size;
 }
 *output = std::move(sorted);
 return true;
}

// A block crossing an owner boundary must be split and resolved independently:
// adjacent guest ranges are allowed to have unrelated physical GPU addresses.
// Missing backing is failure, never a fabricated valid address.
inline bool ResolveGuestAddress(std::span<const GuestAddressRange> table,
                                uint64_t guest, uint64_t bytes, uint64_t* device) {
 if (!device || !bytes || guest >= GuestAddressLimit ||
     bytes > GuestAddressLimit - guest) return false;
 auto it=std::upper_bound(table.begin(),table.end(),guest,
  [](uint64_t address,const GuestAddressRange& range){return address<range.guest_base;});
 if(it==table.begin()) return false;
 --it;
 const auto offset=guest-it->guest_base;
 if(offset>=it->size || bytes>it->size-offset ||
    offset>UINT64_MAX-it->device_base) return false;
 *device=it->device_base+offset;
 return true;
}
} // namespace Libs::Graphics
