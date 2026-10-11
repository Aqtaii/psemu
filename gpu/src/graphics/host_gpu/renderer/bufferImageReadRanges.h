#pragma once
#include <cstdint>
#include <limits>
#include <span>

namespace Libs::Graphics {
struct GpuWrittenBytes { uint64_t address=0,size=0; };
// Tracker-page overlap is not byte ownership. Reject missing/invalid write
// intervals and prove every exact GPU write is outside the sampled image.
inline bool GpuWritesAreOutsideImage(uint64_t address,uint64_t size,
                                     std::span<const GpuWrittenBytes> writes) noexcept {
 if(!address || !size || size>std::numeric_limits<uint64_t>::max()-address || writes.empty())
  return false;
 for(const auto& write:writes) {
  if(!write.address || !write.size ||
     write.size>std::numeric_limits<uint64_t>::max()-write.address) return false;
  if(address<write.address+write.size && write.address<address+size) return false;
 }
 return true;
}
}
