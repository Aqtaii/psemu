#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace Libs::Graphics {
struct GuestAddressFaultRecord {
 uint32_t status = 0;
 uint32_t guest_low = 0;
 uint32_t guest_high = 0;
 uint32_t reserved = 0;
 uint64_t Address() const {return uint64_t(guest_low) | (uint64_t(guest_high)<<32);}
};
static_assert(sizeof(GuestAddressFaultRecord)==16);
// Status remains distinct from payload: an all-zero success has no fault.
// Unknown statuses/invalid addresses must be diagnosed, never populated.
inline bool CanPrepareGuestAddressFault(const GuestAddressFaultRecord& fault) {
 return fault.status==1 && fault.reserved==0 && fault.Address()!=0 &&
        fault.Address()<(uint64_t{1}<<48) && (fault.guest_low&3)==0;
}
}
