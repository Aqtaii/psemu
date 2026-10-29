#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include <cstdio>
#include <stdexcept>
using namespace Libs::Graphics;
static void Require(bool ok) {if(!ok) throw std::runtime_error("depth compatibility regression");}
static HW::DepthZInfo DecodeDepth(uint32_t raw) {
 HW::DepthZInfo z;
 z.tile_mode_index=KYTY_PM4_GET(raw,DB_Z_INFO,TILE_MODE_INDEX);
 z.embedded_sample_locations=KYTY_PM4_GET(raw,DB_Z_INFO,ITERATE_FLUSH)!=0;
 z.plane_compression=KYTY_PM4_GET(raw,DB_Z_INFO,DECOMPRESS_ON_N_ZPLANES);
 return z;
}
static HW::DepthStencilInfo DecodeStencil(uint32_t raw) {
 HW::DepthStencilInfo s;
 s.tile_mode_index=KYTY_PM4_GET(raw,DB_STENCIL_INFO,TILE_MODE_INDEX);
 s.texture_compatible_stencil=KYTY_PM4_GET(raw,DB_STENCIL_INFO,ITERATE_FLUSH)!=0;
 return s;
}
int main() {
 unsigned depth=0,stencil=0;
 for(uint32_t tile=0;tile<8;tile++) for(uint32_t flush=0;flush<2;flush++) for(uint32_t planes=0;planes<16;planes++) {
  const uint32_t raw=(tile<<20)|(flush<<11)|(planes<<23);
  const bool expected=raw==0||raw==0x02900800u;
  const auto decoded=DecodeDepth(raw);
  Require(decoded.HasValidTextureCompatibility()==expected);
  if(expected) depth++;
 }
 for(uint32_t tile=0;tile<8;tile++) for(uint32_t flush=0;flush<2;flush++) {
  const uint32_t raw=(tile<<20)|(flush<<11);
  const bool expected=raw==0||raw==0x00100800u;
  Require(DecodeStencil(raw).HasValidTextureCompatibility()==expected);
  if(expected) stencil++;
 }
 Require(depth==2&&stencil==2);
 for(uint32_t sample=0;sample<4;sample++) for(uint32_t mip=0;mip<16;mip++) {
  Require(DecodeDepth(0xa2900803u|(sample<<2)|(mip<<16)).HasValidTextureCompatibility());
  Require(DecodeStencil(0x20100801u).HasValidTextureCompatibility());
 }
 auto z=DecodeDepth(0x02900800u);z.tile_mode_index=8;Require(!z.HasValidTextureCompatibility());
 z=DecodeDepth(0x02900800u);z.plane_compression=255;Require(!z.HasValidTextureCompatibility());
 auto s=DecodeStencil(0x00100800u);s.tile_mode_index=0xffffffffu;Require(!s.HasValidTextureCompatibility());
 std::puts("PS5 depth/stencil compatibility: PASS, exactly 2 depth and 2 stencil encodings");
}
