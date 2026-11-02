#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <stdexcept>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include "graphics/host_gpu/renderer/metadataClearRange.h"
using namespace Libs::Graphics;
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
#define EXIT(...) throw std::runtime_error("production guard")
struct TestLock {bool held=false;void lock(){CHECK(!held);held=true;}void unlock(){CHECK(held);held=false;}};
struct FaultSafeTextureLock {
 TestLock& lock;
 FaultSafeTextureLock(const void*,TestLock& value):lock(value){lock.lock();}
 ~FaultSafeTextureLock(){lock.unlock();}
};
struct Tracker {
 TestLock *resource=nullptr,*texture=nullptr;
 struct Flags {bool cpu=true,gpu=false;};
 std::map<uint64_t,Flags> pages;
 struct Write {uint64_t address,size;};std::vector<Write> writes;
 template<class F,class G> void ForEachUploadRange(uint64_t a,uint64_t n,bool written,F f,G g){
  CHECK(resource->held && texture->held && written);
  writes.push_back({a,n});f(a,n);g();
  for(auto p=a;p<a+n;p+=TRACKER_PAGE_SIZE)pages[p]={false,true};
 }
 bool IsRegionGpuModified(uint64_t a,uint64_t n){
  for(auto p=a;p<a+n;p+=TRACKER_PAGE_SIZE)if(!pages[p].gpu)return false;return true;
 }
 bool IsRegionCpuModified(uint64_t a,uint64_t n){
  for(auto p=a;p<a+n;p+=TRACKER_PAGE_SIZE)if(pages[p].cpu)return true;return false;
 }
};
struct TextureCache {
 struct Meta {uint64_t size=0;uint32_t layers=1,clear_mask=0;bool gpu_modified=false;};
 struct Image {struct {uint64_t address=0;struct {uint64_t total_bytes=0;} layout;} dcc;};
 std::map<uint64_t,Meta> m_surface_metas;
 std::vector<std::unique_ptr<Image>> m_images;
 TestLock m_resource_mutex,m_lock;
 Tracker m_metadata_tracker;
 TextureCache(){m_metadata_tracker.resource=&m_resource_mutex;m_metadata_tracker.texture=&m_lock;}
 bool ResolveMetaClearRangeLocked(uint64_t,uint64_t,MetadataClearRange*) const;
 bool IsMetaClearRange(uint64_t,uint64_t);bool ClearMetaRange(uint64_t,uint64_t);
 bool IsMetaCleared(uint64_t,uint32_t);
};
#include "metadata_slice_clear_production.inc"
constexpr uint64_t base=0x1888f1e0000,plane=0x40000;
void Seed(TextureCache& c,uint32_t mask){
 c.m_surface_metas[base]={plane*3,3,mask,mask!=0};
 for(uint32_t i=0;i<3;++i)for(auto a=base+plane*i;a<base+plane*(i+1);a+=TRACKER_PAGE_SIZE)
  c.m_metadata_tracker.pages[a]={(mask&(1u<<i))==0,(mask&(1u<<i))!=0};
}
int main(){
 MetadataClearRange p;
 for(uint32_t first=0;first<3;++first)for(uint32_t count=1;count<=3-first;++count){
  CHECK(PlanMetadataClearRange(base,plane*3,3,base+plane*first,plane*count,&p));
  CHECK(p.owner_address==base && p.first_layer==first && p.layer_count==count);
  CHECK(p.clear_mask==(((1u<<count)-1)<<first));
  for(uint32_t mask=0;mask<8;++mask){
   TextureCache c;Seed(c,mask);
   CHECK(c.IsMetaClearRange(base+plane*first,plane*count));
   CHECK(c.m_metadata_tracker.writes.empty());
   CHECK(c.ClearMetaRange(base+plane*first,plane*count));
   CHECK(c.m_surface_metas.at(base).clear_mask==(mask|p.clear_mask));
   const auto& writes=c.m_metadata_tracker.writes;
   CHECK(writes.size()==1 && writes[0].address==base+plane*first && writes[0].size==plane*count);
   for(uint32_t i=0;i<3;++i){
    const bool gpu=((mask|p.clear_mask)&(1u<<i))!=0;
    CHECK(c.IsMetaCleared(base,i)==gpu);
    CHECK(c.m_metadata_tracker.IsRegionGpuModified(base+plane*i,plane)==gpu);
    CHECK(c.m_metadata_tracker.IsRegionCpuModified(base+plane*i,plane)==!gpu);
   }
  }
 }
 CHECK(PlanMetadataClearRange(base,plane*32,32,base,plane*32,&p) && p.clear_mask==UINT32_MAX);
 CHECK(PlanMetadataClearRange(base,plane*32,32,base+plane*31,plane,&p) && p.clear_mask==0x80000000u);
 const auto saved=p;
 for(auto pair:std::vector<std::pair<uint64_t,uint64_t>>{
    {base,0},{base+1,plane},{base,plane-1},{base,plane+1},{base-plane,plane},
    {base+plane*3,plane},{base,UINT64_MAX},{TRACKER_ADDRESS_SIZE,plane}}){
  CHECK(!PlanMetadataClearRange(base,plane*3,3,pair.first,pair.second,&p));
  TextureCache c;Seed(c,6);
  CHECK(!c.IsMetaClearRange(pair.first,pair.second));
  CHECK(!c.ClearMetaRange(pair.first,pair.second));
  CHECK(c.m_metadata_tracker.writes.empty() && c.m_surface_metas.at(base).clear_mask==6);
 }
 CHECK(!PlanMetadataClearRange(base,plane*3,0,base,plane,&p));
 CHECK(!PlanMetadataClearRange(base,plane*3,33,base,plane,&p));
 CHECK(!PlanMetadataClearRange(base,plane*3+1,3,base,plane,&p));
 CHECK(!PlanMetadataClearRange(base+1,plane*3,3,base+1,plane,&p));
 CHECK(!PlanMetadataClearRange(base,0x6000,3,base,0x2000,&p));
 CHECK(!PlanMetadataClearRange(TRACKER_ADDRESS_SIZE-plane,plane*3,3,base,plane,&p));
 CHECK(!PlanMetadataClearRange(base,plane*3,3,base,plane,nullptr));
 CHECK(p.owner_address==saved.owner_address && p.clear_mask==saved.clear_mask);
 for(bool dcc:{false,true}){
  TextureCache c;Seed(c,6);
  if(dcc){auto image=std::make_unique<TextureCache::Image>();
   image->dcc.address=base+plane;image->dcc.layout.total_bytes=plane;c.m_images.push_back(std::move(image));}
  else c.m_surface_metas[base+plane]={plane,1,0,false};
  CHECK(!c.IsMetaClearRange(base+plane,plane));
  CHECK(!c.ClearMetaRange(base+plane,plane));
  CHECK(c.m_metadata_tracker.writes.empty() && c.m_surface_metas.at(base).clear_mask==6);
 }
 {TextureCache c;Seed(c,6);CHECK(c.IsMetaClearRange(base+plane,plane));
  c.m_surface_metas[base+plane]={plane,1,0,false};CHECK(!c.ClearMetaRange(base+plane,plane));
  CHECK(c.m_metadata_tracker.writes.empty() && c.m_surface_metas.at(base).clear_mask==6);}
 puts("PASS: production metadata lookup/transaction, 48 layer-mask ownership cases, exact slice writes, ambiguity/range/47-bit guards; native GPU not executed");
}
