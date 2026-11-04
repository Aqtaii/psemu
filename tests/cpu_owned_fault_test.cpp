// Execute the production HandleFault body against queue and ownership fixtures.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"FAIL %s\n",#x);std::abort();}}while(0)
#define EXIT(...) std::abort()
#define PRIx64 "llx"
constexpr uint64_t TRACKER_PAGE_SIZE=16384;
enum class PageFaultAccess {Read,Write};
static bool queue_waiting=false, paused=false, command_thread=false,label_callback=false;
static unsigned pauses=0,resolutions=0,reads=0,checks=0;
struct ResourceMutex {
 bool owned=false;
 bool IsOwnedByCurrentThread(){return owned;}
 struct FaultScope {
  ResourceMutex& owner;
  explicit FaultScope(ResourceMutex& m):owner(m){CHECK(!m.owned);m.owned=true;}
  ~FaultScope(){CHECK(owner.owned);owner.owned=false;}
 };
};
static ResourceMutex* resource=nullptr;
static bool LabelInCallback(){return label_callback;}
struct CommandProcessor {void BeginReadbackTransaction(){}void EndReadbackTransaction(){}};
static CommandProcessor cp;
static CommandProcessor* GraphicsRunCurrentCommandProcessor(){return command_thread?&cp:nullptr;}
struct GraphicsRunSubmissionLock {
 GraphicsRunSubmissionLock(){CHECK(!resource->owned);CHECK(!queue_waiting);paused=true;++pauses;}
 ~GraphicsRunSubmissionLock(){paused=false;}
};
struct PageManager {
 bool registered=true,full_page=true;uint64_t last_page=0;
 bool IsMapped(uint64_t p,uint64_t size){if(size==1)return registered;CHECK(resource->owned);last_page=p;CHECK(size==TRACKER_PAGE_SIZE);return full_page;}
 bool HandleFault(PageFaultAccess,uint64_t){CHECK(resource->owned);++resolutions;return true;}
};
struct BufferCache {
 bool gpu=false;
 bool IsRegionGpuModified(uint64_t p,uint64_t size){CHECK(resource->owned);CHECK((p&(TRACKER_PAGE_SIZE-1))==0);CHECK(size==TRACKER_PAGE_SIZE);++reads;return gpu;}
};
struct TextureCache {
 struct RegionInfo{bool image_pages=false,metadata_pages=false;}region;
 RegionInfo QueryRegion(uint64_t p,uint64_t size){CHECK(resource->owned);CHECK((p&(TRACKER_PAGE_SIZE-1))==0);CHECK(size==TRACKER_PAGE_SIZE);++checks;return region;}
};
struct GpuResourceManager {
 PageManager m_page_manager;ResourceMutex m_resource_mutex;BufferCache m_buffer_cache;TextureCache m_texture_cache;
 bool HandleFault(PageFaultAccess,uint64_t) noexcept;
};
#include "cpu_owned_fault.inc"
static unsigned cases=0;
static void Run(const char* name,bool gpu,bool image,bool metadata,bool full,bool waiting,PageFaultAccess access){
 GpuResourceManager manager;resource=&manager.m_resource_mutex;
 manager.m_buffer_cache.gpu=gpu;manager.m_texture_cache.region={image,metadata};manager.m_page_manager.full_page=full;
 queue_waiting=waiting;pauses=resolutions=reads=checks=0;
 CHECK(manager.HandleFault(access,0x13fff));
 CHECK(resolutions==1 && !paused && !resource->owned);
 const bool ordered=gpu||image||metadata||!full;
 CHECK(pauses==unsigned(ordered));
 if(full)CHECK(manager.m_page_manager.last_page==0x10000);
 ++cases;std::printf("PASS %s\n",name);
}
int main(){
 Run("CPU read while graphics awaits CPU signal",false,false,false,true,true,PageFaultAccess::Read);
 Run("CPU write while graphics awaits CPU signal",false,false,false,true,true,PageFaultAccess::Write);
 Run("CPU clean ordinary read",false,false,false,true,false,PageFaultAccess::Read);
 Run("GPU dirty buffer retains drain",true,false,false,true,false,PageFaultAccess::Read);
 Run("GPU dirty write retains drain",true,false,false,true,false,PageFaultAccess::Write);
 Run("image page retains drain",false,true,false,true,false,PageFaultAccess::Read);
 Run("image page write retains drain",false,true,false,true,false,PageFaultAccess::Write);
 Run("metadata page retains drain",false,false,true,true,false,PageFaultAccess::Write);
 Run("partial registration retains drain",false,false,false,false,false,PageFaultAccess::Read);
 {GpuResourceManager manager;resource=&manager.m_resource_mutex;manager.m_page_manager.registered=false;pauses=resolutions=0;
 CHECK(!manager.HandleFault(PageFaultAccess::Read,0x13fff));CHECK(pauses==0&&resolutions==0);++cases;}
 {GpuResourceManager manager;resource=&manager.m_resource_mutex;command_thread=true;queue_waiting=true;pauses=resolutions=0;
 CHECK(manager.HandleFault(PageFaultAccess::Read,0x13fff));CHECK(pauses==0&&resolutions==1&&!resource->owned);command_thread=false;++cases;}
 std::printf("%u production fault scenarios passed\n",cases);
}
