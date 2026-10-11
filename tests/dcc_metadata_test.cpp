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
 std::vector<DccSlice> snapshot{{DccSliceKind::Clear,{1,1,1,1}}};
 unsigned reads=0;bool correct_address=true;
 auto reader=[&](uint64_t address,void* out,size_t count) {
  ++reads;correct_address=address==0x18617918000ull;
  std::fill_n(static_cast<uint8_t*>(out),count,uint8_t(0xff));return true;
 };
 CHECK(ReadDccSlices(0x18617918000ull,unchanged,true,reader,&snapshot));
 CHECK(correct_address && reads==1 && snapshot.size()==1 && snapshot[0].kind==DccSliceKind::Expanded);
 CHECK(!ReadDccSlices(0,unchanged,true,reader,&snapshot));CHECK(reads==1);
 CHECK(!ReadDccSlices(0x18617918000ull,unchanged,true,
  [](uint64_t,void*,size_t){return false;},&snapshot));
 CHECK(snapshot.size()==1 && snapshot[0].kind==DccSliceKind::Expanded);
 DccSlice black{DccSliceKind::Clear,{0,0,0,0}};
 CHECK(CanInitializeDccClear(black,1,0,1,true,true));
 for(auto color:std::vector<std::array<float,4>>{{1,1,1,0},{0,0,0,1},{1,1,1,1}}) {
  CHECK(CanInitializeDccClear({DccSliceKind::Clear,color},1,0,1,true,true));
 }
 CHECK(!CanInitializeDccClear({DccSliceKind::Expanded,{}},1,0,1,true,true));
 CHECK(!CanInitializeDccClear({},1,0,1,true,true));
 CHECK(!CanInitializeDccClear(black,2,0,1,true,true));
 CHECK(!CanInitializeDccClear(black,1,1,1,true,true));
 CHECK(!CanInitializeDccClear(black,1,0,2,true,true));
 CHECK(!CanInitializeDccClear(black,1,0,1,false,true));
 CHECK(!CanInitializeDccClear(black,1,0,1,true,false));
 CHECK(!CanInitializeDccClear({DccSliceKind::Clear,{0.5f,0,0,0}},1,0,1,true,true));
 CHECK(!CanInitializeDccClear({DccSliceKind::Clear,{std::numeric_limits<float>::quiet_NaN(),0,0,0}},1,0,1,true,true));
 // Captured color extent and metadata footprint must agree before native mutation.
 DccBinding binding{0x18617918000ull,unchanged,true};
 CHECK(ValidRgba16fDccBinding(binding,1920,1080));
 CHECK(!ValidRgba16fDccBinding(binding,512,256));
 auto altered=binding;altered.layout.total_bytes=4096;
 CHECK(!ValidRgba16fDccBinding(altered,1920,1080));
 altered=binding;altered.layout.block_height*=2;
 CHECK(!ValidRgba16fDccBinding(altered,1920,1080));
 CHECK(SameDccBinding(binding,binding));
 altered=binding;altered.address+=4096;CHECK(!SameDccBinding(binding,altered));
 altered=binding;altered.alpha_msb=false;CHECK(!SameDccBinding(binding,altered));
 altered=binding;altered.layout.slice_bytes=4096;CHECK(!SameDccBinding(binding,altered));
 // A fresh full snapshot observes a rewritten clear key; stale snapshots cannot be memoized.
 std::vector<uint8_t> keys(unchanged.total_bytes,0);
 auto key_reader=[&](uint64_t,void* out,size_t count) {
  std::copy_n(keys.data(),count,static_cast<uint8_t*>(out));return true;
 };
 CHECK(ReadDccSlices(binding.address,binding.layout,true,key_reader,&snapshot));
 CHECK(snapshot[0].kind==DccSliceKind::Clear);
 std::fill(keys.begin(),keys.end(),0xff);
 CHECK(ReadDccSlices(binding.address,binding.layout,true,key_reader,&snapshot));
 CHECK(snapshot[0].kind==DccSliceKind::Expanded);
 std::fill(keys.begin(),keys.end(),0x40);
 CHECK(ReadDccSlices(binding.address,binding.layout,true,key_reader,&snapshot));
 CHECK(snapshot[0].kind==DccSliceKind::Clear && snapshot[0].color[3]==1.f);
 keys.back()=0xff;
 CHECK(ReadDccSlices(binding.address,binding.layout,true,key_reader,&snapshot));
 CHECK(snapshot[0].kind==DccSliceKind::Unsupported);
 std::puts("PASS: DCC footprints, range limits, complete-slice clears and expanded/mixed metadata");
}
