#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
#define EXIT_IF(x) do {if(x)throw std::runtime_error("production guard");} while(0)
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);} while(0)
namespace vk {enum class ImageLayout {eUndefined,ePreinitialized,eGeneral};}
uint32_t VulkanFormat(uint32_t f){return f;}
struct ImageInfo {
 uint64_t address=0x10000,size=0x20000;uint32_t format=22,tile=24,width=128,height=128,pitch=128,
  levels=1,view_levels=1,base_level=0,type=9,depth=1;bool dirty=false;
 bool IsCpuDirty()const{return dirty;}
};
struct NativeImage {
 uint32_t format=22;struct {uint32_t width=128,height=128;} extent;
 uint32_t layers=1,mip_levels=1;vk::ImageLayout layout=vk::ImageLayout::eGeneral;
};
struct GraphicContext {};
struct Mutex {bool owned=true;bool IsOwnedByCurrentThread(){return owned;}};
struct Tracker {
 bool gpu=true,cpu=false;int downloads=0;
 bool IsRegionGpuModified(uint64_t,uint64_t){return gpu;}
 bool IsRegionCpuModified(uint64_t,uint64_t){return cpu;}
 template<bool B,class F>void ForEachDownloadRange(uint64_t,uint64_t,F f){++downloads;}
};
struct BufferCache {
 bool gpu=false,cpu=false,present=false;int publications=0;
 bool IsRegionGpuModified(uint64_t,uint64_t){return gpu;}
 bool IsCachedRegionCpuModified(uint64_t,uint64_t){return cpu;}
 bool HasPageOverlap(uint64_t,uint64_t){return present;}
 void PublishReconstructedImageBacking(uint64_t,uint64_t){++publications;}
};
struct PolicyCall {bool gpu,buffer,tracker,cpu,context,pages,buffers;};
std::vector<PolicyCall> policy_calls;
bool CanPublishStorageForBufferRead(uint64_t,uint64_t,const ImageInfo&,bool gpu,bool buffer,
 bool tracker,bool cpu,bool context,bool pages,bool buffers) {
 policy_calls.push_back({gpu,buffer,tracker,cpu,context,pages,buffers});
 return gpu && !buffer && tracker && !cpu && context && !pages && !buffers;
}
struct TextureCache {
 struct CachedImage {
  enum class Kind {StorageTexture,RenderTarget,DepthTarget}kind=Kind::StorageTexture;
  bool gpu_modified=true,buffer_modified=false,page_conflict=false;
  ImageInfo info;GraphicContext* ctx=nullptr;NativeImage storage;NativeImage* image=&storage;
  struct {uint32_t format=0,tile_mode=0,width=0,height=0,pitch=0,bytes_per_element=0,levels=0,layers=0;}target;
  struct {uint32_t format=0,tile_mode=0,width=0,height=0,pitch=0;uint64_t stencil_address=0,stencil_size=0,htile_address=0,htile_size=0;}depth;
  uint64_t Address()const{return info.address;}uint64_t Size()const{return info.size;}
 };
 struct Readback {
  TextureCache* cache;int calls=0;
  struct Range {uint64_t address,size;};
  Range DownloadColorImage(CachedImage& c,bool padding) {
   CHECK(padding);++calls;
   if(cache->m_buffer_cache.present)++cache->m_buffer_cache.publications;
   return {c.Address(),c.Size()};
  }
 };
 Mutex m_resource_mutex;int m_lock=0;Tracker m_memory_tracker;BufferCache m_buffer_cache;
 Readback worker{this};Readback* m_readback=&worker;
 bool metadata=false;std::vector<std::unique_ptr<CachedImage>> images;
 bool HasMetaOverlapLocked(uint64_t,uint64_t){return metadata;}
 bool HasOtherImagePageOwnerLocked(CachedImage& c){return c.page_conflict;}
 std::vector<CachedImage*> FindImagesInRegionLocked(uint64_t,uint64_t,bool){
  std::vector<CachedImage*> result;for(auto& c:images)result.push_back(c.get());return result;
 }
 bool PublishStorageForBufferRead(GraphicContext*,uint64_t,uint64_t);
};
struct FaultSafeTextureLock {FaultSafeTextureLock(TextureCache*,int&){}};
#include "storage_buffer_read_production.inc"
struct Fixture {
 GraphicContext ctx;TextureCache cache;
 Fixture(){policy_calls.clear();auto image=std::make_unique<TextureCache::CachedImage>();image->ctx=&ctx;cache.images.push_back(std::move(image));}
 auto& image(){return *cache.images.front();}
 bool Run(){return cache.PublishStorageForBufferRead(&ctx,0x10001,17);}
 void NoMutation(){CHECK(cache.worker.calls==0 && cache.m_memory_tracker.downloads==0 && cache.m_buffer_cache.publications==0);CHECK(image().gpu_modified);}
};
int main(){
 for(bool fragments:{false,true}) {
  Fixture f;f.cache.m_buffer_cache.present=fragments;auto* original=f.image().image;
  CHECK(f.Run());CHECK(f.cache.worker.calls==1 && f.cache.m_memory_tracker.downloads==1);
  CHECK(f.cache.m_buffer_cache.publications==1 && !f.image().gpu_modified);
  CHECK(!f.image().buffer_modified && f.image().image==original);
  CHECK(!f.Run());CHECK(f.cache.worker.calls==1);
 }
 for(int bad=0;bad<15;++bad) {
  Fixture f;
  switch(bad){
   case 0:f.image().buffer_modified=true;break;case 1:f.cache.m_memory_tracker.gpu=false;break;
   case 2:f.cache.m_memory_tracker.cpu=true;break;case 3:f.image().info.dirty=true;break;
   case 4:f.image().ctx=nullptr;break;case 5:f.image().page_conflict=true;break;
   case 6:f.cache.m_buffer_cache.gpu=true;break;case 7:f.cache.m_buffer_cache.cpu=true;break;
   case 8:f.image().kind=TextureCache::CachedImage::Kind::DepthTarget;break;
   case 9:f.image().storage.format=71;break;case 10:f.image().storage.extent.width++;break;
   case 11:f.image().storage.layers=2;break;case 12:f.image().storage.mip_levels=2;break;
   case 13:f.image().storage.layout=vk::ImageLayout::eUndefined;break;
   case 14:f.cache.metadata=true;break;
  }
  CHECK(!f.Run());f.NoMutation();
 }
 {Fixture f;auto second=std::make_unique<TextureCache::CachedImage>();second->ctx=&f.ctx;
  second->info.address=0x40000;second->buffer_modified=true;f.cache.images.push_back(std::move(second));
  CHECK(!f.Run());CHECK(policy_calls.size()==2);f.NoMutation();}
 {Fixture f;f.cache.m_resource_mutex.owned=false;bool rejected=false;try{f.Run();}catch(const std::runtime_error&){rejected=true;}CHECK(rejected);f.NoMutation();}
 puts("Storage buffer read: extracted production transaction, clean/native retention, all-owner preflight and 15 rejection states PASS");
}