#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <utility>
struct PublicationTracker {
 std::vector<std::pair<uint64_t,uint64_t>> cpu,gpu,published;
 static bool Overlaps(const auto& ranges,uint64_t address,uint64_t size) {
  for(const auto& [begin,length]:ranges)if(address<begin+length && begin<address+size)return true;
  return false;
 }
 bool IsRegionCpuModified(uint64_t address,uint64_t size){return Overlaps(cpu,address,size);}
 bool IsRegionGpuModified(uint64_t address,uint64_t size){return Overlaps(gpu,address,size);}
 void MarkRegionAsCpuModified(uint64_t address,uint64_t size){published.emplace_back(address,size);}
 template<class R,class U> void ForEachUploadRange(uint64_t address,uint64_t size,bool written,R range,U upload) {
  if(written)throw std::runtime_error("unexpected GPU ownership");
  decltype(cpu) keep;
  for(const auto& [begin,bytes]:cpu) {
   auto lo=std::max(begin,address),hi=std::min(begin+bytes,address+size);
   if(lo>=hi){keep.emplace_back(begin,bytes);continue;}
   range(lo,hi-lo);
   if(begin<lo)keep.emplace_back(begin,lo-begin);
   if(hi<begin+bytes)keep.emplace_back(hi,begin+bytes-hi);
  }
  cpu=std::move(keep);upload();
 }
};
struct PublicationRanges {
 std::vector<std::pair<uint64_t,uint64_t>> dirty;
 auto Intersections(uint64_t address,uint64_t size) const {
  return PublicationTracker::Overlaps(dirty,address,size)?dirty:decltype(dirty){};
 }
};
struct GraphicContext {};
struct VulkanBuffer {
 void* buffer=reinterpret_cast<void*>(1);
 uint64_t buffer_size=0;
 std::vector<uint8_t> bytes;
};
namespace Libs::LibKernel::Memory {
 inline uint64_t fixture_base=0;
 inline std::vector<uint8_t> fixture_backing;
 inline bool TryReadBacking(uint64_t address,void* output,uint64_t size) {
  if(address<fixture_base || address-fixture_base>fixture_backing.size() ||
     size>fixture_backing.size()-(address-fixture_base))return false;
  std::memcpy(output,fixture_backing.data()+address-fixture_base,size);return true;
 }
}
namespace Transfer {
 enum class StagingBufferType {Vertex};
 inline size_t fixture_uploads=0;
 inline void UploadBuffer(GraphicContext*,StagingBufferType,VulkanBuffer* target,uint64_t offset,
                          const void* data,uint64_t size) {
  if(offset>target->bytes.size() || size>target->bytes.size()-offset)throw std::runtime_error("upload escapes buffer");
  std::memcpy(target->bytes.data()+offset,data,size);fixture_uploads++;
 }
}
struct BufferCache {
 struct CachedBuffer {
  uint64_t size;
  GraphicContext* ctx=nullptr;
  std::shared_ptr<VulkanBuffer> buffer;
 };
 int m_mutex=0;
 std::map<uint64_t,std::unique_ptr<CachedBuffer>> m_buffers;
 PublicationTracker m_memory_tracker;
 PublicationRanges m_gpu_modified_ranges;
 void RefreshCpuImageFragmentsLocked(uint64_t,uint64_t);
 bool IsCachedRegionCpuModified(uint64_t,uint64_t);
 void PublishReconstructedImageBacking(uint64_t,uint64_t);
 void ReconstructImageBackingPair(uint64_t,uint64_t,uint64_t,uint64_t,const std::function<void()>&);
 void ReconstructImageBacking(uint64_t,uint64_t,const std::function<void()>&,uint64_t pending_fault_page=0);
};
struct FaultSafeCacheLock {FaultSafeCacheLock(BufferCache*,int&){}};
