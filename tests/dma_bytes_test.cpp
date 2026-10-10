#include "graphics/host_gpu/renderer/dmaBytes.h"
#include <array>
#include <cstdio>
#include <vector>
#include <stdexcept>
using namespace Libs::Graphics;
static void Check(bool ok) {if(!ok)throw std::runtime_error("DMA regression failed");}
int main() {
 for(size_t length:std::array<size_t,11>{0,1,2,3,4,5,7,15,16,17,4097}) {
  for(size_t offset=1;offset<=4;++offset) {
   std::vector<uint8_t> data(length+offset+8,0xcd);
   FillDmaBytes(data.data()+offset,length,0x44332211u);
   for(size_t i=0;i<data.size();++i)
    Check(data[i]==(i>=offset&&i<offset+length?std::array<uint8_t,4>{0x11,0x22,0x33,0x44}[(i-offset)%4]:0xcd));
  }
 }
 Check(IsValidDmaCopyRange(0x2001,0x1003,5));
 Check(!IsValidDmaCopyRange(0x1002,0x1000,5));
 Check(!IsValidDmaCopyRange(UINT64_MAX-2,0x1000,3));
 Check(!IsValidDmaCopyRange(0x1000,UINT64_MAX-2,3));
 Check(!IsValidDmaCopyRange(0,0x1000,1));
 CommandProcessor cp;
 for(uint32_t count:std::array<uint32_t,5>{1,3,5,7,4097}) {
  cp.DmaData(0,0,0,0x2001,0,0,0x1003,count,1,1,0);
  Check(resources.kind==1&&resources.size==count&&resources.src==0x1003&&resources.dst==0x2001);
  cp.DmaData(0,0,0,0x2001,2,0,0x44332211,count,1,1,0);
  Check(resources.kind==2&&resources.size==count&&resources.value==0x44332211);
 }
 resources.kind=0;
 cp.DmaData(0,0,0,0x2001,0,0,0x1003,0,1,1,0);
 Check(resources.kind==0);
 bool rejected=false;
 try {cp.DmaData(0,0,0,0x2001,1,0,0x1003,5,1,1,0);}
 catch(const std::runtime_error&) {rejected=true;}
 Check(rejected);
 std::puts("PASS: byte DMA dispatch, fill boundaries and copy range validation");
}
