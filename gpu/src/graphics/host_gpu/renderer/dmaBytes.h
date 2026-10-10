#ifndef PSEMU_DMA_BYTES_H
#define PSEMU_DMA_BYTES_H
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace Libs::Graphics {
[[nodiscard]] inline constexpr bool IsValidDmaCopyRange(uint64_t dst, uint64_t src, uint64_t size) noexcept {
 return dst != 0 && src != 0 && size != 0 && size <= UINT64_MAX-dst &&
        size <= UINT64_MAX-src && !(src < dst+size && dst < src+size);
}
inline void FillDmaBytes(void* destination, size_t size, uint32_t value) noexcept {
 auto* dst=static_cast<uint8_t*>(destination);
 size_t offset=0;
 for(;size-offset>=sizeof(value);offset+=sizeof(value)) {
  std::memcpy(dst+offset,&value,sizeof(value));
 }
 if(offset<size) std::memcpy(dst+offset,&value,size-offset);
}
}
#endif
