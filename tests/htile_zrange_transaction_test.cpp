#include "graphics/host_gpu/renderer/htileZRange.h"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <limits>
using namespace Libs::Graphics;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
constexpr uint64_t TRACKER_ADDRESS_SIZE=1ull<<47;
struct TestLock {bool held=false;void lock(){CHECK(!held);held=true;}void unlock(){CHECK(held);held=false;}};
static TestLock* resource_lock=nullptr;static TestLock* texture_lock=nullptr;
static int drains=0,waits=0,downloads=0;
static void GraphicsRunSynchronizeCommandProcessors(){CHECK(!resource_lock->held && !texture_lock->held);++drains;}
namespace vk {
enum class Format {eD32Sfloat=126,eD32SfloatS8Uint=130,eOther=97};
enum class ImageLayout {eUndefined,ePreinitialized,eGeneral};
enum class ImageAspectFlagBits {eDepth};
}
namespace Prospero {
enum class BufferFormat{k32Float=22};enum class TileMode{kDepth=24};
template<class T> uint32_t GpuEnumValue(T v){return static_cast<uint32_t>(v);}
}
struct GraphicContext{};
struct NativeImage {
 vk::Format format=vk::Format::eD32SfloatS8Uint;
 struct {uint32_t width=8,height=8;}extent;
 uint32_t layers=1,mip_levels=1;vk::ImageLayout layout=vk::ImageLayout::eGeneral;
 std::vector<float> depth=std::vector<float>(64,.75f);
};
namespace Transfer {
struct Copies {uint32_t layers;uint64_t bytes;uint32_t width,pitch,height;vk::ImageAspectFlagBits aspect;};
void WaitForGraphicsIdle(GraphicContext*){CHECK(!resource_lock->held && !texture_lock->held && drains==waits+1);++waits;}
Copies MakeLayeredImageBufferCopies(uint32_t l,uint64_t b,uint32_t w,uint32_t p,uint32_t h,vk::ImageAspectFlagBits a){return {l,b,w,p,h,a};}
void DownloadImage(GraphicContext*,void* out,uint64_t bytes,const Copies& r,NativeImage* image,vk::ImageLayout restore){
 CHECK(resource_lock->held && texture_lock->held && drains==waits);
 CHECK(r.layers==1 && r.bytes==bytes && r.width==image->extent.width && r.pitch==r.width);
 CHECK(r.height==image->extent.height && r.aspect==vk::ImageAspectFlagBits::eDepth);
 CHECK(bytes==image->depth.size()*4 && restore==image->layout);++downloads;
 std::memcpy(out,image->depth.data(),bytes);
}
}
static uint32_t pitch_word=1024;static bool pitch_readable=true;
namespace LibKernel::Memory {
bool TryReadBacking(uint64_t,void* out,size_t bytes){
 CHECK(resource_lock->held && texture_lock->held && bytes==4);
 if(!pitch_readable)return false;std::memcpy(out,&pitch_word,4);return true;
}
}
bool ImagePageRangesOverlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return n && m && a<b+m && b<a+n;}
struct Tracker {
 bool gpu=true,cpu=false;
 bool IsRegionGpuModified(uint64_t,uint64_t){CHECK(resource_lock->held && texture_lock->held);return gpu;}
 bool IsRegionCpuModified(uint64_t,uint64_t){CHECK(resource_lock->held && texture_lock->held);return cpu;}
};
struct BufferCache {
 bool gpu=false,cpu=false;
 bool IsRegionGpuModified(uint64_t,uint64_t){CHECK(resource_lock->held && texture_lock->held);return gpu;}
 bool IsCachedRegionCpuModified(uint64_t,uint64_t){CHECK(resource_lock->held && texture_lock->held);return cpu;}
};
struct TextureCache {
 struct Meta {uint64_t size=32768;uint32_t layers=1;bool gpu_modified=true;};
 struct CachedImage {
  enum class Kind{DepthTarget,Other}kind=Kind::DepthTarget;
  struct {uint64_t address=0;struct{uint64_t total_bytes=0;}layout;}dcc;
  struct {uint64_t address=0x100000,size=0x10000,stencil_address=0x120000,stencil_size=0x1000,
   htile_address=0x200000,htile_size=32768;
   uint32_t width=8,height=8,layers=1,bytes_per_element=4,guest_format=22,tile_mode=24;
   bool stencil_htile_compressed=false;vk::Format format=vk::Format::eD32SfloatS8Uint;
  }depth;
  GraphicContext*ctx=nullptr;NativeImage native;NativeImage*image=&native;
  bool gpu_modified=true,buffer_modified=false,page_conflict=false;
  uint32_t RangeCount()const{return 2;}uint64_t Address(uint32_t p)const{return p?depth.stencil_address:depth.address;}
  uint64_t Size(uint32_t p)const{return p?depth.stencil_size:depth.size;}
  bool OverlapsRange(uint64_t a,uint64_t n,bool)const{
   return ImagePageRangesOverlap(a,n,depth.address,depth.size) || ImagePageRangesOverlap(a,n,depth.stencil_address,depth.stencil_size);}
 };
 TestLock m_resource_mutex,m_lock;Tracker m_metadata_tracker,m_memory_tracker;BufferCache m_buffer_cache;
 std::map<uint64_t,Meta>m_surface_metas;std::vector<std::unique_ptr<CachedImage>>m_images;
 bool pitch_meta=false;
 bool HasOtherImagePageOwnerLocked(const CachedImage& c){return c.page_conflict;}
 bool HasMetaOverlapLocked(uint64_t,uint64_t){return pitch_meta;}
 bool ReadHtileZRangeSnapshot(GraphicContext*,uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,std::vector<uint32_t>*);
};
struct FaultSafeTextureLock {
 TestLock& lock;FaultSafeTextureLock(TextureCache*,TestLock& l):lock(l){CHECK(resource_lock->held);lock.lock();}
 ~FaultSafeTextureLock(){lock.unlock();}
};
#include "htile_zrange_transaction_production.inc"
struct Fixture {
 GraphicContext ctx;TextureCache cache;std::vector<uint32_t>out{0x1234,0x5678};
 Fixture(){
  resource_lock=&cache.m_resource_mutex;texture_lock=&cache.m_lock;drains=waits=downloads=0;
  pitch_word=1024;pitch_readable=true;cache.m_surface_metas[0x200000]={};
  auto image=std::make_unique<TextureCache::CachedImage>();image->ctx=&ctx;cache.m_images.push_back(std::move(image));
 }
 auto& image(){return *cache.m_images.front();}
 bool Run(uint64_t a=0x200000,uint64_t n=32768,uint64_t pitch=0x300000,uint32_t gx=1,uint32_t gy=1){
  return cache.ReadHtileZRangeSnapshot(&ctx,a,n,pitch,gx,gy,&out);
 }
 void CheckUnchanged(){
  CHECK(!cache.m_resource_mutex.held && !cache.m_lock.held && drains==waits);
  CHECK(image().native.layout==vk::ImageLayout::eGeneral);
  CHECK(cache.m_surface_metas.at(0x200000).gpu_modified && cache.m_metadata_tracker.gpu && cache.m_memory_tracker.gpu);
 }
};
int main(){try{
 for(auto format:{vk::Format::eD32Sfloat,vk::Format::eD32SfloatS8Uint}){
  Fixture f;f.image().depth.format=f.image().native.format=format;
  const auto before=f.image().native.depth;CHECK(f.Run());CHECK(downloads==1 && f.out.size()==8192);
  CHECK(f.image().native.depth==before && f.image().gpu_modified && !f.image().buffer_modified);
  CHECK(((f.out[0]>>4)&16383)<=.75*16383 && (f.out[0]>>18)>=.75*16383);
  f.CheckUnchanged();
 }
 for(int bad=0;bad<29;++bad){
  Fixture f;
  switch(bad){
   case 0:f.cache.m_surface_metas.at(0x200000).layers=2;break;
   case 1:f.cache.m_metadata_tracker.gpu=false;break;case 2:f.cache.m_metadata_tracker.cpu=true;break;
   case 3:f.cache.m_surface_metas[0x200004]={};break;
   case 4:f.image().dcc.address=0x200000;f.image().dcc.layout.total_bytes=4;break;
   case 5:{auto duplicate=std::make_unique<TextureCache::CachedImage>();duplicate->ctx=&f.ctx;f.cache.m_images.push_back(std::move(duplicate));break;}
   case 6:f.image().ctx=nullptr;break;case 7:f.image().gpu_modified=false;break;
   case 8:f.image().buffer_modified=true;break;case 9:f.image().depth.htile_size=65536;break;
   case 10:f.image().depth.layers=2;break;case 11:f.image().depth.bytes_per_element=2;break;
   case 12:f.image().depth.guest_format=1;break;case 13:f.image().depth.tile_mode=27;break;
   case 14:f.image().depth.stencil_htile_compressed=true;break;case 15:f.image().depth.format=vk::Format::eOther;break;
   case 16:f.image().native.extent.width=16;break;case 17:f.image().native.layers=2;break;
   case 18:f.image().native.mip_levels=2;break;case 19:f.image().native.layout=vk::ImageLayout::eUndefined;break;
   case 20:f.image().page_conflict=true;break;case 21:f.cache.m_memory_tracker.cpu=true;break;
   case 22:f.cache.m_buffer_cache.gpu=true;break;case 23:f.cache.m_buffer_cache.cpu=true;break;
   case 24:f.cache.m_memory_tracker.gpu=false;break;case 25:f.cache.pitch_meta=true;break;
   case 26:pitch_readable=false;break;case 27:pitch_word=0;break;
   case 28:f.image().native.depth[19]=std::numeric_limits<float>::quiet_NaN();break;
  }
  const auto before=f.out;const bool image_gpu=f.image().gpu_modified,buffer=f.image().buffer_modified;
  CHECK(!f.Run() && f.out==before);CHECK(downloads==(bad==28?1:0));
  CHECK(f.image().gpu_modified==image_gpu && f.image().buffer_modified==buffer);
  CHECK(!f.cache.m_resource_mutex.held && !f.cache.m_lock.held && drains==waits);
 }
 {Fixture f;CHECK(!f.Run(0x200000,32764) && downloads==0);}
 {Fixture f;CHECK(!f.Run(0x200000,32768,0x300000,2,1) && downloads==0);}
 {Fixture f;CHECK(!f.Run(0x200000,32768,0x300001) && drains==0);}
 {Fixture f;CHECK(!f.Run(TRACKER_ADDRESS_SIZE-4,32768) && drains==0);}
 puts("PASS extracted HTile snapshot transaction: depth-only/D32S8, producer drain before nonrecursive locks, immutable owners/backing, 33 rejection cases");
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL HTile transaction: %s\n",e.what());return 1;}}
