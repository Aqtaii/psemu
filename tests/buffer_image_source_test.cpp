int main(){
 try {
  using namespace Libs::LibKernel::Memory;
  GraphicContext ctx;BufferCache cache;
  auto native=std::make_shared<VulkanBuffer>();native->buffer_size=0x4000;native->bytes=backing;
  auto owner=std::make_unique<BufferCache::CachedBuffer>();owner->vaddr=base;owner->size=0x4000;owner->ctx=&ctx;owner->buffer=native;
  cache.m_buffers.emplace(base,std::move(owner));
  // Captured Astro sub-page shape: +0x800, size0x1000. Resolve both edge pages,
  // including newer GPU bytes before/after the requested texture's exact range.
  for(uint64_t off:{0x100,0x900,0x1a00,0x2100}){
   std::fill_n(native->bytes.begin()+off,16,static_cast<uint8_t>(off>>8));
   cache.m_gpu_modified_ranges.Add(base+off,16);
   cache.m_memory_tracker.gpu.Add(base+(off&~4095ull),4096);
  }
  auto source=cache.ObtainBufferForImage(base+0x800,0x1000);
  Require(source.buffer==native.get()&&source.offset==0x800&&source.address==base+0x800&&source.size==0x1000&&source.cpu_current&&!source.cpu_dirty);
  Require(Transfer::downloads==3&&Transfer::waits==1);
  for(uint64_t i=0;i<0x4000;i++){
   uint8_t expected=0xa5;
   for(uint64_t off:{0x100,0x900,0x1a00})if(i>=off&&i<off+16)expected=static_cast<uint8_t>(off>>8);
   Require(backing[i]==expected);
  }
  Require(cache.m_gpu_modified_ranges.Intersections(base,0x2000).empty());
  Require(cache.m_gpu_modified_ranges.Intersections(base+0x2000,0x1000).size()==1);
  Require(cache.m_memory_tracker.IsRegionGpuModified(base+0x2000,4096));
  // A one-byte request must also preserve the other dirty bytes of its page.
  source=cache.ObtainBufferForImage(base+0x2001,1);
  Require(source.offset==0x2001&&source.size==1&&Transfer::downloads==4);
  Require(cache.m_gpu_modified_ranges.Empty());
  Require(backing[0x2100]==0x21);
  // CPU-dirty edge pages refresh the full cached pages, preserving exact view.
  std::fill(backing.begin(),backing.begin()+0x2000,0x72);
  cache.m_memory_tracker.cpu.Add(base,0x2000);
  source=cache.ObtainBufferForImage(base+0x800,0x1000);
  Require(source.cpu_dirty&&source.offset==0x800&&source.size==0x1000);
  Require(Transfer::uploads==1);
  for(size_t i=0;i<0x2000;i++)Require(native->bytes[i]==0x72);
  Require(native->bytes[0x2000]==0xa5);
  // Existing aligned callers keep their extent and avoid redundant transfers.
  auto old_downloads=Transfer::downloads,old_uploads=Transfer::uploads;
  source=cache.ObtainBufferForImage(base,0x1000);
  Require(source.buffer==native.get()&&source.offset==0&&source.size==0x1000&&!source.cpu_dirty);
  Require(Transfer::downloads==old_downloads&&Transfer::uploads==old_uploads);
  // Two resident fragments with a cache hole cannot return a containing buffer.
  // Both complete edge pages refresh, while CPU ownership in the hole remains.
  BufferCache split;
  for(uint64_t off:{0,0x2000}){
   auto fragment=std::make_unique<BufferCache::CachedBuffer>();fragment->vaddr=base+off;
   fragment->size=4096;fragment->ctx=&ctx;fragment->buffer=std::make_shared<VulkanBuffer>();
   fragment->buffer->buffer_size=4096;fragment->buffer->bytes.resize(4096,0);
   split.m_buffers.emplace(base+off,std::move(fragment));
  }
  split.m_memory_tracker.cpu.Add(base,0x3000);
  auto fragmented=split.ObtainBufferForImage(base+0x800,0x2000);
  Require(!fragmented.buffer&&fragmented.offset==0&&fragmented.address==base+0x800&&fragmented.size==0x2000&&fragmented.cpu_dirty);
  Require(Transfer::uploads==old_uploads+2);
  Require(split.m_memory_tracker.IsRegionCpuModified(base+0x1000,4096));
  Require(!split.m_memory_tracker.IsRegionCpuModified(base,4096)&&!split.m_memory_tracker.IsRegionCpuModified(base+0x2000,4096));
  for(const auto& [a,f]:split.m_buffers)for(size_t i=0;i<4096;i++)Require(f->buffer->bytes[i]==backing[a-base+i]);
  // Cache holes use coherent guest backing; never widen the returned extent.
  BufferCache empty;auto fallback=empty.ObtainBufferForImage(base+0x801,3);
  Require(!fallback.buffer&&fallback.offset==0&&fallback.address==base+0x801&&fallback.size==3&&fallback.cpu_current);
  unsigned rejected=0;
  for(auto [a,n]:std::vector<std::pair<uint64_t,uint64_t>>{{0,1},{base,0},{TRACKER_ADDRESS_SIZE,1},{TRACKER_ADDRESS_SIZE-1,2},{UINT64_MAX-1,4}}){
   try{empty.ObtainBufferForImage(a,n);}catch(const std::runtime_error&){rejected++;}
  }
  Require(rejected==5);
  fallback=empty.ObtainBufferForImage(TRACKER_ADDRESS_SIZE-1,1);
  Require(fallback.address==TRACKER_ADDRESS_SIZE-1&&fallback.size==1);
  std::puts("PASS: production image source sub-page views, GPU edge bytes, CPU page refresh, fallback and bounds");return 0;
 }catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
}
