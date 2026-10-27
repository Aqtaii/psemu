#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
using Libs::Graphics::RangeSet;
constexpr uint64_t TRACKER_PAGE_SIZE=4096,TRACKER_ADDRESS_SIZE=1ull<<47;
#define Require(x) do {if(!(x))throw std::runtime_error("test failure: " #x);}while(0)
struct GraphicContext {};
struct VulkanBuffer {void* buffer=this;uint64_t buffer_size;std::vector<uint8_t> bytes;};
struct BufferImageCopySource {VulkanBuffer* buffer;uint64_t offset,address,size;bool cpu_current,cpu_dirty;};
struct Tracker {
 RangeSet cpu,gpu;
 static void Pages(uint64_t a,uint64_t n){Require(((a|n)&4095)==0);}
 bool IsRegionCpuModified(uint64_t a,uint64_t n){Pages(a,n);return !cpu.Intersections(a,n).empty();}
 bool IsRegionGpuModified(uint64_t a,uint64_t n){Pages(a,n);return !gpu.Intersections(a,n).empty();}
 template<bool clear,class P,class F>void ForEachDownloadRange(uint64_t a,uint64_t n,P pre,F copy){
  Pages(a,n);auto ranges=gpu.Intersections(a,n);
  for(auto r:ranges)pre(r.address,r.size);
  for(auto r:ranges)copy(r.address,r.size);
  if constexpr(clear)gpu.Subtract(a,n);
 }
 template<class F,class U>void ForEachUploadRange(uint64_t a,uint64_t n,bool write,F f,U upload){
  Pages(a,n);Require(!write);for(auto r:cpu.Intersections(a,n))f(r.address,r.size);
  cpu.Subtract(a,n);upload();
 }
};
namespace Libs::LibKernel::Memory {
 inline uint64_t base=0x100000;
 inline std::vector<uint8_t> backing(0x4000,0xa5);
 bool TryReadBacking(uint64_t a,void* p,uint64_t n){Require(a>=base&&a-base+n<=backing.size());std::memcpy(p,backing.data()+a-base,n);return true;}
 void WriteBacking(uint64_t a,const void* p,uint64_t n){Require(a>=base&&a-base+n<=backing.size());std::memcpy(backing.data()+a-base,p,n);}
}
namespace Transfer {
 enum class StagingBufferType {Vertex};
 inline unsigned waits=0,downloads=0,uploads=0;
 void WaitForGraphicsIdle(GraphicContext*){waits++;}
 void DownloadBuffer(GraphicContext*,VulkanBuffer* b,uint64_t off,void* p,uint64_t n){Require(off+n<=b->bytes.size());std::memcpy(p,b->bytes.data()+off,n);downloads++;}
 void UploadBuffer(GraphicContext*,StagingBufferType,VulkanBuffer* b,uint64_t off,const void* p,uint64_t n){Require(off+n<=b->bytes.size());std::memcpy(b->bytes.data()+off,p,n);uploads++;}
}
bool GraphicsRunIsCommandProcessorThread(){return true;}
bool GraphicsRunSubmissionLockHeld(){return false;}
bool LabelInCallback(){return false;}
struct BufferCache {
 struct CachedBuffer {uint64_t vaddr,size;GraphicContext* ctx;std::shared_ptr<VulkanBuffer> buffer;};
 int m_mutex=0,m_page_manager=0;
 std::map<uint64_t,std::unique_ptr<CachedBuffer>>m_buffers;
 Tracker m_memory_tracker;
 RangeSet m_gpu_modified_ranges;
 void RefreshCpuImageFragmentsLocked(uint64_t,uint64_t);
 BufferImageCopySource ObtainBufferForImage(uint64_t,uint64_t);
};
struct FaultSafeCacheLock {FaultSafeCacheLock(BufferCache*,int&){}};
int ReserveBackingWrites(int&,const std::vector<RangeSet::Range>& r){Require(!r.empty());return 1;}
void ValidateDirtyPages(const RangeSet& d,uint64_t a,uint64_t n,const char*){
 Require(((a|n)&4095)==0);for(uint64_t p=a;p<a+n;p+=4096)Require(!d.Intersections(p,4096).empty());
}
