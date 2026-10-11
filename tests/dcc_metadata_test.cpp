#include "graphics/host_gpu/renderer/dccMetadata.h"
#include <cstdio>
#include <algorithm>
#include <vector>
using namespace Libs::Graphics;
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\\n",__LINE__,#x);return 1;}}while(0)
int main() {
 DccLayout layout;
 CHECK(ComputeDccLayout(1920,1080,1,8,1,&layout));
 CHECK(layout.block_width==512 && layout.block_height==256);
 CHECK(layout.slice_bytes==81920 && layout.total_bytes==81920);
 CHECK(ValidDccRange(0x18617918000ull,layout));
 CHECK(!ValidDccRange(0,layout));
 CHECK(!ValidDccRange(0x18617918001ull,layout));
 CHECK(!ValidDccRange((1ull<<48)-4096,layout));
 for(uint32_t bytes:{1u,2u,4u,8u,16u}) {
  DccLayout one,many;
  CHECK(ComputeDccLayout(1,1,1,bytes,1,&one));
  CHECK(one.total_bytes==4096);
  CHECK(ComputeDccLayout(one.block_width+1,one.block_height+1,3,bytes,1,&many));
  CHECK(many.slice_bytes==16384 && many.total_bytes==49152);
 }
 auto unchanged=layout;
 CHECK(!ComputeDccLayout(UINT32_MAX,UINT32_MAX,UINT32_MAX,16,1,&layout));
 CHECK(layout.total_bytes==unchanged.total_bytes);
 CHECK(!ComputeDccLayout(1920,1080,1,3,1,&layout));
 CHECK(!ComputeDccLayout(1920,1080,1,8,2,&layout));
 CHECK(!ComputeDccLayout(0,1080,1,8,1,&layout));
 std::vector<uint8_t> data(81920);
 for(uint8_t code:{uint8_t(0),uint8_t(0x40),uint8_t(0x80),uint8_t(0xc0)}) {
  std::fill(data.begin(),data.end(),code);
  auto clear=InspectDccSlice(data,81920,true);
  CHECK(clear.kind==DccSliceKind::Clear);
  CHECK(clear.color[0]==((code&0x80)?1.f:0.f));
  CHECK(clear.color[3]==((code&0x40)?1.f:0.f));
  auto reversed=InspectDccSlice(data,81920,false);
  CHECK(reversed.color[0]==clear.color[3] && reversed.color[3]==clear.color[0]);
  data.back()^=1;CHECK(InspectDccSlice(data,81920,true).kind==DccSliceKind::Unsupported);
 }
 std::fill(data.begin(),data.end(),0xff);
 CHECK(InspectDccSlice(data,81920,true).kind==DccSliceKind::Expanded);
 data.back()=0;CHECK(InspectDccSlice(data,81920,true).kind==DccSliceKind::Unsupported);
 std::fill(data.begin(),data.end(),0x20);
 CHECK(InspectDccSlice(data,81920,true).kind==DccSliceKind::Unsupported);
 CHECK(InspectDccSlice(data,4096,true).kind==DccSliceKind::Unsupported);
 CHECK(InspectDccSlice({},0,true).kind==DccSliceKind::Unsupported);
 std::puts("PASS: DCC footprints, range limits, complete-slice clears and expanded/mixed metadata");
}
