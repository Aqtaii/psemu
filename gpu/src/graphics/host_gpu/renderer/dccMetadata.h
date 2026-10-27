#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include <utility>

namespace Libs::Graphics {
// Original psemu adaptation of pinned KytyPS5 tile.cpp::TileGetDccSize
// and textureCache.cpp::DecodeColorClear/MaterializeColorClear semantics.
// This describes metadata; callers must synchronize backing before inspection.
struct DccLayout {
 uint32_t block_width=0,block_height=0,slice_bytes=0,total_bytes=0;
};
inline bool ComputeDccLayout(uint32_t width,uint32_t height,uint32_t slices,
                             uint32_t element_bytes,uint32_t levels,DccLayout* out) {
 if(!out || !width || !height || !slices || levels!=1 ||
    !std::has_single_bit(element_bytes) || element_bytes>16) return false;
 const auto bits=20u-std::countr_zero(element_bytes);
 const auto bw=1u<<((bits+1u)/2u),bh=1u<<(bits/2u);
 const uint64_t nx=(uint64_t(width)+bw-1)/bw,ny=(uint64_t(height)+bh-1)/bh;
 const uint64_t blocks=nx*ny;
 if(blocks>std::numeric_limits<uint32_t>::max()/4096u/slices) return false;
 *out={bw,bh,uint32_t(blocks*4096u),uint32_t(blocks*4096u*slices)};
 return true;
}
inline bool ValidDccRange(uint64_t address,const DccLayout& layout) {
 constexpr uint64_t end=uint64_t{1}<<48;
 return address && !(address&4095u) && address<end && layout.total_bytes &&
        layout.slice_bytes && !(layout.slice_bytes&4095u) &&
        layout.total_bytes%layout.slice_bytes==0 && layout.total_bytes<=end-address;
}
struct DccBinding {
 uint64_t address=0;
 DccLayout layout{};
 bool alpha_msb=true;
};
inline bool SameDccBinding(const DccBinding& a,const DccBinding& b) {
 return a.address==b.address && a.layout.total_bytes==b.layout.total_bytes &&
        a.layout.slice_bytes==b.layout.slice_bytes && a.alpha_msb==b.alpha_msb;
}
inline bool ValidColorDccBinding(const DccBinding& binding,uint32_t width,uint32_t height,uint32_t element_bytes) {
 DccLayout expected;
 return (element_bytes==4 || element_bytes==8) && ValidDccRange(binding.address,binding.layout) &&
        ComputeDccLayout(width,height,1,element_bytes,1,&expected) &&
        binding.layout.block_width==expected.block_width &&
        binding.layout.block_height==expected.block_height &&
        binding.layout.slice_bytes==expected.slice_bytes &&
        binding.layout.total_bytes==expected.total_bytes;
}
inline bool ValidRgba16fDccBinding(const DccBinding& binding,uint32_t width,uint32_t height) {
 return ValidColorDccBinding(binding,width,height,8);
}
enum class DccSliceKind { Unsupported, Expanded, Clear };
struct DccSlice {
 DccSliceKind kind=DccSliceKind::Unsupported;
 std::array<float,4> color{};
};
// Native initialization is deliberately narrower than metadata inspection: no arrays,
// mip chains, register clears, or arbitrary colors can reach the transfer-clear path.
inline bool CanInitializeDccClear(const DccSlice& slice,uint32_t levels,uint32_t base_level,
                                  uint32_t depth,bool color_2d,bool supported_color) {
 if(slice.kind!=DccSliceKind::Clear || levels!=1 || base_level || depth!=1 ||
    !color_2d || !supported_color) return false;
 for(float lane:slice.color) if(lane!=0.f && lane!=1.f) return false;
 return true;
}
// Inspect the entire slice, not only its first byte. Register clears (0x20)
// require render-target clear registers and remain unsupported here.
inline DccSlice InspectDccSlice(std::span<const uint8_t> bytes,uint32_t expected_bytes,
                                bool alpha_msb) {
 if(!expected_bytes || (expected_bytes&4095u) || bytes.size()!=expected_bytes) return {};
 const auto code=bytes.front();
 for(auto byte:bytes) if(byte!=code) return {};
 if(code==0xff) return {DccSliceKind::Expanded,{}};
 if(code!=0 && code!=0x40 && code!=0x80 && code!=0xc0) return {};
 const float rgb=(code&0x80)?1.f:0.f,alpha=(code&0x40)?1.f:0.f;
 auto color=std::array<float,4>{rgb,rgb,rgb,alpha};
 if(!alpha_msb) {color[0]=alpha;color[3]=rgb;}
 return {DccSliceKind::Clear,color};
}
template<class Reader>
bool ReadDccSlices(uint64_t address,const DccLayout& layout,bool alpha_msb,
                   Reader&& reader,std::vector<DccSlice>* output) {
 if(!output || !ValidDccRange(address,layout)) return false;
 std::vector<uint8_t> bytes(layout.total_bytes);
 if(!reader(address,bytes.data(),bytes.size())) return false;
 std::vector<DccSlice> staged;
 staged.reserve(layout.total_bytes/layout.slice_bytes);
 for(size_t offset=0;offset<bytes.size();offset+=layout.slice_bytes)
  staged.push_back(InspectDccSlice(std::span<const uint8_t>(bytes).subspan(offset,layout.slice_bytes),
                                  layout.slice_bytes,alpha_msb));
 *output=std::move(staged);
 return true;
}

}
