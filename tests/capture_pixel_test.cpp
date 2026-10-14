#include "../gpu/adapter/capture_pixel.h"
#include <cassert>
using namespace Libs::Graphics;
int main(){
 const uint8_t bytes[]={10,20,30,40};
 assert((CapturePixelBgr(bytes,CapturePixelFormat::Bgra8)==std::array<uint8_t,3>{10,20,30}));
 assert((CapturePixelBgr(bytes,CapturePixelFormat::Rgba8)==std::array<uint8_t,3>{30,20,10}));
 for(uint32_t alpha=0;alpha<4;++alpha) {
  uint32_t packed=(alpha<<30)|(1023u<<20)|(512u<<10)|1u;
  auto* p=reinterpret_cast<uint8_t*>(&packed);
  assert((CapturePixelBgr(p,CapturePixelFormat::Bgr10)==std::array<uint8_t,3>{0,128,255}));
  assert((CapturePixelBgr(p,CapturePixelFormat::Rgb10)==std::array<uint8_t,3>{255,128,0}));
 }
 uint32_t white=0x3fffffffu,black=0xc0000000u;
 assert((CapturePixelBgr(reinterpret_cast<uint8_t*>(&white),CapturePixelFormat::Bgr10)==std::array<uint8_t,3>{255,255,255}));
 assert((CapturePixelBgr(reinterpret_cast<uint8_t*>(&black),CapturePixelFormat::Rgb10)==std::array<uint8_t,3>{0,0,0}));
}