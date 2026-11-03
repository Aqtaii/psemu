// Production ObtainBuffer preparation prefix, with a retained foreign partial
// owner. Models recording resets and CPU stream snapshots; no game/GPU assets.
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
#define EXIT(...) throw std::runtime_error("production guard")
namespace vk {using DeviceSize=uint64_t;}
struct CommandBuffer {
 int queue=8;uint64_t generation=1;
 bool IsInvalid()const{return false;}bool IsExecute()const{return false;}
 int GetQueue()const{return queue;}uint64_t GetRecordingGeneration()const{return generation;}
};
struct GraphicContext {
 static constexpr int QUEUES_NUM=11;
 struct {uint32_t family=0;} queues[QUEUES_NUM];
 uint64_t StorageMinAlignment()const{return 256;}
};
struct VulkanBuffer{void* buffer=reinterpret_cast<void*>(1);};
struct BufferCacheRange{uint64_t address,size;};
static uint32_t resource_depth=0,drains=0,idles=0,copies=0,backing_reads=0;
static bool readable=true,uploadable=true;
static uint64_t request_address=0;
static CommandBuffer* active_command=nullptr;
struct ResourceMutex {
 std::recursive_mutex mutex;
 void lock(){mutex.lock();++resource_depth;}void unlock(){--resource_depth;mutex.unlock();}
};
struct TextureRegion{bool gpu_image_bytes=false,gpu_metadata_bytes=false;};
struct TextureCache{
 TextureRegion region;
 TextureRegion QueryRegion(uint64_t,uint64_t){CHECK(resource_depth);return region;}
 bool PublishImageForBufferRead(GraphicContext*,uint64_t,uint64_t){
  CHECK(resource_depth);region.gpu_image_bytes=false;return true;
 }
};
struct MemoryTracker{
 bool cpu=true,gpu=false;
 bool IsRegionGpuModified(uint64_t,uint64_t)const{return gpu;}
 bool IsRegionCpuModified(uint64_t,uint64_t)const{return cpu;}
};
static std::array<uint8_t,16384> backing{};
static std::array<uint8_t,32768> stream_bytes{};
namespace Libs::LibKernel::Memory {
 static bool TryReadBacking(uint64_t address,void* out,uint64_t size){
  CHECK(resource_depth && address==request_address);++backing_reads;
  if(!readable)return false;CHECK(size<=backing.size());std::memcpy(out,backing.data(),size);return true;
 }
}
struct FaultSafeCacheLock{
 std::lock_guard<std::mutex> held;
 template<class T>FaultSafeCacheLock(T*,std::mutex& m):held(m){}
};
static void GraphicsRunSynchronizeCommandProcessors(){
 CHECK(resource_depth==0);++drains;++active_command->generation;
}
namespace Transfer {static void WaitForGraphicsIdle(GraphicContext*){CHECK(resource_depth==0);++idles;}}
struct CachedBuffer{
 uint64_t vaddr=0,size=0,queue_mask=0;GraphicContext*ctx=nullptr;
 std::shared_ptr<VulkanBuffer> buffer=std::make_shared<VulkanBuffer>();
};
class BufferCache {
public:
 static constexpr uint64_t CACHING_PAGE_SIZE=16384;
 ResourceMutex m_resource_mutex;std::mutex m_mutex;
 MemoryTracker m_memory_tracker;TextureCache texture;TextureCache*m_texture_cache=&texture;
 std::map<uint64_t,std::unique_ptr<CachedBuffer>> m_buffers;
 VulkanBuffer streamed;
 static uint64_t AlignDown(uint64_t n){return n&~(CACHING_PAGE_SIZE-1);}
 static uint64_t AlignUp(uint64_t n){return (n+CACHING_PAGE_SIZE-1)&~(CACHING_PAGE_SIZE-1);}
 void ValidateGpuAccess(uint64_t,uint64_t,bool,bool){}
 bool UploadHostData(CommandBuffer*,GraphicContext*,const void*src,uint64_t size,uint64_t alignment,
  VulkanBuffer** out,uint64_t*offset,uint64_t*range){
  CHECK(resource_depth && alignment==256);
  if(!uploadable)return false;++copies;
  std::memcpy(stream_bytes.data()+256,src,size);*out=&streamed;*offset=256;*range=size;return true;
 }
 std::pair<VulkanBuffer*,uint64_t> CompleteResident(GraphicContext*ctx,uint64_t begin,uint64_t end){
  CHECK(resource_depth);m_buffers.clear();auto b=std::make_unique<CachedBuffer>();
  b->vaddr=begin;b->size=end-begin;b->ctx=ctx;b->queue_mask=uint64_t{1}<<active_command->queue;
  auto* native=b->buffer.get();m_buffers.emplace(begin,std::move(b));return {native,0};
 }
 std::pair<VulkanBuffer*,uint64_t> ObtainFixed(CommandBuffer*,GraphicContext*,uint64_t,uint64_t,bool,bool,bool,bool);
 std::pair<VulkanBuffer*,uint64_t> ObtainOld(CommandBuffer*,GraphicContext*,uint64_t,uint64_t,bool,bool,bool,bool);
};
#include "buffer_stream_preparation.inc"
struct Options {
 bool written=false,read=true,force=false,cpu=true,gpu=false,image=false,metadata=false,read_ok=true,upload_ok=true;
 uint64_t address=0x13f78,size=0xc0;
};
static void Case(const char*name,bool fixed,Options o={},bool stalled=false,bool expected_stream=true,uint32_t expected_drains=0){
 resource_depth=drains=idles=copies=backing_reads=0;readable=o.read_ok;uploadable=o.upload_ok;
 request_address=o.address;stream_bytes.fill(0xcd);
 CommandBuffer command;active_command=&command;GraphicContext ctx;BufferCache cache;
 cache.m_memory_tracker={o.cpu,o.gpu};cache.texture.region={o.image,o.metadata};
 auto old=std::make_unique<CachedBuffer>();old->vaddr=0x10000;old->size=0x4000;old->queue_mask=1ull<<4;old->ctx=&ctx;
 const auto old_native=old->buffer;cache.m_buffers.emplace(old->vaddr,std::move(old));
 std::pair<VulkanBuffer*,uint64_t> result;uint32_t rounds=0;bool stable=false;
 for(;rounds<12;++rounds){
  const auto generation=command.generation;
  result=fixed?cache.ObtainFixed(&command,&ctx,o.address,o.size,o.written,o.read,false,o.force):
               cache.ObtainOld(&command,&ctx,o.address,o.size,o.written,o.read,false,o.force);
  if(generation==command.generation){stable=true;++rounds;break;}
 }
 CHECK(stable!=stalled && resource_depth==0 && drains==idles);
 if(stalled){CHECK(rounds==12 && drains==12 && copies==12);}
 else {
  CHECK(drains==expected_drains);
  CHECK((result.first==&cache.streamed)==expected_stream);
 }
 if(expected_stream){
  CHECK(result.first==&cache.streamed && result.second==256);
  for(size_t i=0;i<stream_bytes.size();++i)
   CHECK(stream_bytes[i]==(i>=256 && i<256+o.size?backing[i-256]:0xcd));
  CHECK(cache.m_buffers.size()==1 && cache.m_buffers.begin()->second->buffer==old_native);
  CHECK(cache.m_buffers.begin()->second->queue_mask==(1ull<<4));
  CHECK(cache.m_memory_tracker.cpu==o.cpu && cache.m_memory_tracker.gpu==o.gpu);
 } else CHECK(copies==0);
 std::printf("[BUFFER-STREAM] %s fixed=%u stable=%u rounds=%u drains=%u copies=%u exact_snapshot_guards=1 game_frame=0\n",
  name,unsigned(fixed),unsigned(stable),rounds,drains,copies);
}
int main(){
 try{
  for(size_t i=0;i<backing.size();++i)backing[i]=uint8_t(i*73+5);
  Case("old-cross-page-retry-control",false,{},true);
  Case("cross-page-foreign-owner",true);
  Options o;o.address=0x10180;Case("contained-read",true,o);
  o={};o.force=true;Case("forced-residency",true,o,false,false,1);
  o={};o.gpu=true;Case("gpu-current-buffer",true,o,false,false,1);
  o={};o.cpu=false;Case("clean-cpu-tracker",true,o,false,false,1);
  o={};o.written=true;Case("writable-buffer",true,o,false,false,1);
  o={};o.read=false;Case("not-readable-binding",true,o,false,false,1);
  o={};o.size=16385;Case("oversized-read",true,o,false,false,1);
  o={};o.metadata=true;Case("gpu-metadata-owner",true,o,false,false,1);
  o={};o.image=true;Case("publish-image-before-stream",true,o,false,true,1);
  o={};o.read_ok=false;Case("failed-backing-read",true,o,false,false,1);
  o={};o.upload_ok=false;Case("failed-stream-upload",true,o,false,false,1);
  std::puts("PASS production stream/preparation prefix: old retry control, exact 192-byte cross-page snapshot, unchanged foreign owner/tracker, preserved drain and residency guards");
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
