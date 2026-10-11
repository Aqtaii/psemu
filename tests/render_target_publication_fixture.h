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
};
struct PublicationRanges {
 std::vector<std::pair<uint64_t,uint64_t>> dirty;
 auto Intersections(uint64_t address,uint64_t size) const {
  return PublicationTracker::Overlaps(dirty,address,size)?dirty:decltype(dirty){};
 }
};
struct BufferCache {
 struct CachedBuffer {uint64_t size;};
 int m_mutex=0;
 std::map<uint64_t,std::unique_ptr<CachedBuffer>> m_buffers;
 PublicationTracker m_memory_tracker;
 PublicationRanges m_gpu_modified_ranges;
 void PublishReconstructedImageBacking(uint64_t,uint64_t);
 void ReconstructImageBacking(uint64_t,uint64_t,const std::function<void()>&,uint64_t pending_fault_page=0);
};
struct FaultSafeCacheLock {FaultSafeCacheLock(BufferCache*,int&){}};
