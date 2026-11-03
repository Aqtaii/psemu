#include <algorithm>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
#define EXIT(...) throw std::runtime_error("production ownership guard")
constexpr uint64_t TRACKER_ADDRESS_SIZE=1ull<<47;
struct TestLock{
 bool held=false;void lock(){CHECK(!held);held=true;}void unlock(){CHECK(held);held=false;}
 bool IsOwnedByCurrentThread()const{return held;}
};
struct DepthTargetInfo{uint64_t address=0x100000,size=0x870000,stencil_address=0xa00000,stencil_size=0x280000;};
struct VulkanImage{};struct DepthStencilVulkanImage:VulkanImage{};
struct CommandBuffer{int retained=0;template<class T>void RetainResourceUntilFence(const T&){++retained;}};
struct TextureCache{
 struct CachedImage{
  enum class Kind{DepthTarget,Other}kind=Kind::DepthTarget;
  DepthTargetInfo depth;bool stencil_initialized=true,buffer_modified=false;
  DepthStencilVulkanImage native;VulkanImage*image=&native;
  bool OverlapsRange(uint64_t a,uint64_t n,bool)const{
   const auto overlap=[&](uint64_t b,uint64_t m){return a<b+m && b<a+n;};
   return overlap(depth.address,depth.size)||overlap(depth.stencil_address,depth.stencil_size);
  }
 };
 TestLock m_resource_mutex,m_lock;
 struct Tracker{bool cpu=false;TextureCache*c;bool IsRegionCpuModified(uint64_t,uint64_t){CHECK(c->m_resource_mutex.held&&c->m_lock.held);return cpu;}}m_memory_tracker{false,this};
 struct Buffer{bool gpu=false,cpu=false;TextureCache*c;
  bool IsRegionGpuModified(uint64_t,uint64_t){CHECK(c->m_resource_mutex.held&&c->m_lock.held);return gpu;}
  bool IsCachedRegionCpuModified(uint64_t,uint64_t){CHECK(c->m_resource_mutex.held&&c->m_lock.held);return cpu;}
 }m_buffer_cache{false,false,this};
 std::vector<std::shared_ptr<CachedImage>>m_images;
 std::vector<CachedImage*>FindImagesInRegionLocked(uint64_t,uint64_t,bool){
  CHECK(m_resource_mutex.held&&m_lock.held);std::vector<CachedImage*> out;for(auto& p:m_images)out.push_back(p.get());return out;
 }
 DepthStencilVulkanImage*FindDepthTargetByRange(CommandBuffer*,uint64_t,uint64_t,bool,bool*);
};
struct FaultSafeTextureLock{
 TestLock&lock;FaultSafeTextureLock(TextureCache*c,TestLock&v):lock(v){CHECK(c->m_resource_mutex.held);lock.lock();}
 ~FaultSafeTextureLock(){lock.unlock();}
};
#include "stencil_owner_production.inc"
struct Fixture{
 TextureCache c;CommandBuffer command;bool stencil=false;
 Fixture(){c.m_images.push_back(std::make_shared<TextureCache::CachedImage>());}
 auto& owner(){return *c.m_images.front();}
 auto Run(uint64_t a=0xa00000,uint64_t n=0x280000){return c.FindDepthTargetByRange(&command,a,n,true,&stencil);}
};
int main(){try{
 for(bool parent:{false,true}){
  Fixture f;if(parent)f.c.m_resource_mutex.lock();
  CHECK(f.Run()==&f.owner().native && f.stencil && f.command.retained==1);
  CHECK(f.c.m_resource_mutex.held==parent && !f.c.m_lock.held);
  CHECK(f.owner().stencil_initialized && !f.owner().buffer_modified);
  if(parent)f.c.m_resource_mutex.unlock();
 }
 {Fixture f;f.stencil=true;CHECK(f.Run(0x100000,0x870000)==&f.owner().native && !f.stencil && f.command.retained==1);}
 {Fixture f;f.stencil=true;CHECK(f.Run(0x2000000,4096)==nullptr && f.stencil && f.command.retained==0);}
 for(int bad=0;bad<10;++bad){
  Fixture f;uint64_t a=0xa00000,n=0x280000;
  switch(bad){
   case 0:f.owner().stencil_initialized=false;break;
   case 1:f.owner().buffer_modified=true;break;case 2:f.c.m_memory_tracker.cpu=true;break;
   case 3:f.c.m_buffer_cache.gpu=true;break;case 4:f.c.m_buffer_cache.cpu=true;break;
   case 5:f.c.m_images.push_back(std::make_shared<TextureCache::CachedImage>());break;
   case 6:n--;break;case 7:a++;n--;break;case 8:n=0;break;
   case 9:a=TRACKER_ADDRESS_SIZE-32;n=4096;break;
  }
  const bool initialized=f.owner().stencil_initialized,buffer=f.owner().buffer_modified;
  bool rejected=false;try{f.Run(a,n);}catch(const std::runtime_error&){rejected=true;}
  CHECK(rejected && !f.stencil && f.command.retained==0);
  CHECK(!f.c.m_lock.held && !f.c.m_resource_mutex.held);
  CHECK(f.owner().stencil_initialized==initialized && f.owner().buffer_modified==buffer);
 }
 puts("PASS extracted sampled-stencil ownership: exact initialized plane, ambiguity/dirty/range rejection, retained owner, caller-held transaction preserved");
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL stencil owner: %s\n",e.what());return 1;}}
