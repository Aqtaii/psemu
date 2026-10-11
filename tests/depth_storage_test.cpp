static void Require(bool ok) {if(!ok) throw std::runtime_error("regression failed");}
int main() {
 RenderTargetInfo old_target{},new_target{};
 old_target.address=0x1c5e8a80000ull;old_target.size=0xff0000;
 new_target.address=0x1c5e9770000ull;new_target.size=0x480000;
 Require(CanRetireBufferOwnedRenderTarget(old_target,false,true,true,true,new_target));
 Require(!CanRetireBufferOwnedRenderTarget(old_target,true,true,true,true,new_target));
 Require(!CanRetireBufferOwnedRenderTarget(old_target,false,false,true,true,new_target));
 Require(!CanRetireBufferOwnedRenderTarget(old_target,false,true,false,true,new_target));
 Require(!CanRetireBufferOwnedRenderTarget(old_target,false,true,true,false,new_target));
 auto bad_target=new_target;bad_target.address++;
 Require(!CanRetireBufferOwnedRenderTarget(old_target,false,true,true,true,bad_target));
 bad_target=new_target;bad_target.address=old_target.address+old_target.size;
 Require(!CanRetireBufferOwnedRenderTarget(old_target,false,true,true,true,bad_target));
 // Full reconstructed image publication spans multiple buffer fragments but not external bytes.
 for(bool competing_gpu:{false,true}) for(bool competing_cpu:{false,true}) {
  BufferCache cache;
  cache.m_buffers.emplace(0x10000,std::make_unique<BufferCache::CachedBuffer>(BufferCache::CachedBuffer{0x10000}));
  cache.m_buffers.emplace(0x40000,std::make_unique<BufferCache::CachedBuffer>(BufferCache::CachedBuffer{0x20000}));
  cache.m_memory_tracker.gpu={{0x60000,0x10000}}; // adjacent GPU data must survive
  if(competing_gpu)cache.m_gpu_modified_ranges.dirty={{0x40000,0x1000}};
  if(competing_cpu)cache.m_memory_tracker.cpu={{0x40000,0x1000}};
  bool rejected=false,reconstructed=false;
  try {cache.ReconstructImageBacking(0x10000,0x50000,[&]{
   Require(cache.m_memory_tracker.published.empty());
   reconstructed=true;
  });}
  catch(const std::runtime_error&){rejected=true;}
  Require(rejected==(competing_gpu || competing_cpu));
  Require(reconstructed==!rejected);
  Require(cache.m_memory_tracker.published.size()==(rejected?0u:1u));
  if(!rejected)Require(cache.m_memory_tracker.published[0]==std::pair<uint64_t,uint64_t>{0x10000,0x50000});
  Require(cache.m_memory_tracker.gpu[0]==std::pair<uint64_t,uint64_t>{0x60000,0x10000});
 }
 // A read fault's active page must stay pending until Complete/Release, while
 // all other cached fragments become dirty before submissions resume.
 {
  BufferCache cache;
  cache.m_buffers.emplace(0x10000,std::make_unique<BufferCache::CachedBuffer>(BufferCache::CachedBuffer{0x50000}));
  bool reconstructed=false;
  cache.ReconstructImageBacking(0x10000,0x50000,[&]{reconstructed=true;},0x30000);
  Require(reconstructed);
  Require(cache.m_memory_tracker.published.size()==2);
  Require(cache.m_memory_tracker.published[0]==std::pair<uint64_t,uint64_t>{0x10000,0x20000});
  Require(cache.m_memory_tracker.published[1]==std::pair<uint64_t,uint64_t>{0x31000,0x2f000});
  cache.PublishReconstructedImageBacking(0x30000,0x1000);
  Require(cache.m_memory_tracker.published.back()==std::pair<uint64_t,uint64_t>{0x30000,0x1000});
 }
 // Invalid fault pages must not execute reconstruction or publish any bytes.
 for(uint64_t fault:{uint64_t{0x10001},uint64_t{0xf000},uint64_t{0x60000}}) {
  BufferCache cache;
  bool called=false,rejected=false;
  try {cache.ReconstructImageBacking(0x10000,0x50000,[&]{called=true;},fault);}
  catch(const std::runtime_error&){rejected=true;}
  Require(rejected && !called && cache.m_memory_tracker.published.empty());
 }
 // Captured formatted buffer starts within a GPU target and extends beyond its end.
 constexpr uint64_t image=0x19d6e170000ull,image_size=0xff0000;
 auto classify=[&](uint64_t address,uint64_t size,bool gpu=true,bool yielded=false,bool formatted=true) {
  return ClassifyBufferImageWrite(address,size,image,image_size,BufferImageBinding::RenderTarget,gpu,formatted,yielded);
 };
 Require(classify(0x19d6ee60000ull,0x480000)==BufferImageWrite::SynchronizeRenderTarget);
 Require(classify(image-0x10000,0x20000)==BufferImageWrite::SynchronizeRenderTarget);
 Require(classify(image-0x10000,image_size+0x20000)==BufferImageWrite::SynchronizeRenderTarget);
 Require(classify(image,image_size)==BufferImageWrite::SynchronizeRenderTarget);
 Require(classify(image+image_size,0x1000)==BufferImageWrite::None);
 Require(classify(image+1,0x1000)==BufferImageWrite::Unsupported);
 Require(classify(image,0x1000,true,false,false)==BufferImageWrite::Unsupported);
 Require(classify(image,0x1000,true,true)==BufferImageWrite::Unsupported);
 Require(classify(image,0x1000,false,false)==BufferImageWrite::Unsupported);
 Require(classify(image,0x1000,false,true)==BufferImageWrite::InvalidateRenderTarget);
 // Expanding render-target support must not expand the exact video/depth contract.
 for(auto kind:{BufferImageBinding::VideoOut,BufferImageBinding::DepthTarget})
  Require(ClassifyBufferImageWrite(0x19d6ee60000ull,0x480000,image,image_size,kind,true,true)==BufferImageWrite::Unsupported);
 // Captured Astro failure: BDA snapshot rereads a resident 32 KiB buffer after
 // its GPU write invalidated the overlapping image. Reuse the buffer, not backing.
 Require(IsCoherentBufferImageRead(true,false,true,false,true));
 // An image that did not yield, or a mixed set of yielded/current aliases, must reject.
 Require(!IsCoherentBufferImageRead(true,false,true,false,false));
 // Two GPU owners still conflict; image-only GPU ownership needs real readback.
 Require(!IsCoherentBufferImageRead(true,false,true,true,true));
 Require(!IsCoherentBufferImageRead(true,false,false,true,false));
 // CPU-current overlapping read stays supported; writes use the transition path.
 Require(IsCoherentBufferImageRead(true,false,false,false,false));
 Require(!IsCoherentBufferImageRead(true,true,true,false,true));
 Require(!IsCoherentBufferImageRead(false,false,true,false,true));

 Require(IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16Float),0x22c));
 Require(!IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16Float),DstSel(5,4,0,1)));
 Require(!IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16Float),0x22c));
 for(auto f:std::array<Prospero::BufferFormat,9>{Prospero::BufferFormat::k8_8UNorm,Prospero::BufferFormat::k8_8SNorm,Prospero::BufferFormat::k8_8UInt,Prospero::BufferFormat::k16_16UNorm,Prospero::BufferFormat::k16_16SNorm,Prospero::BufferFormat::k16_16UInt,Prospero::BufferFormat::k16_16Float,Prospero::BufferFormat::k32_32UInt,Prospero::BufferFormat::k32_32Float}) {
  Require(IsSupportedStorageSwizzle(Prospero::GpuEnumValue(f),0x22c));
  Require(!IsSupportedStorageSwizzle(Prospero::GpuEnumValue(f),DstSel(5,4,0,1)));
 }
 ImageInfo info{};
 info.address=0x10000;info.size=0x280000;info.width=960;info.height=540;info.pitch=1024;
 info.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
 info.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
 info.tile=Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
 Require(IsSupportedDepthStorageBacking(info));
 for(int change=0;change<8;++change) {
  auto bad=info;
  switch(change) {case 0:bad.size-=4;break;case 1:bad.pitch=960;break;
   case 2:bad.address+=4;break;case 3:bad.levels=2;break;case 4:bad.depth=2;break;
   case 5:bad.base_level=1;break;case 6:bad.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k16Float);break;
   case 7:bad.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray);break;}
  Require(!IsSupportedDepthStorageBacking(bad));
 }
 for(auto extent:std::array<std::array<uint32_t,2>,3>{{{960,540},{129,131},{1,1}}}) {
  auto width=extent[0],height=extent[1],pitch=(width+127)&~127u;
  auto words=static_cast<size_t>(pitch)*((height+127)&~127u);
  constexpr uint32_t guard=0xdeadbeef;
  std::vector<uint32_t> linear(words+2,guard),tiled(words+2,guard),back(words+2,guard);
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
   linear[1+static_cast<size_t>(y)*pitch+x]=0x3f000000u^(x*0x9e3779b9u)^(y*0x85ebca6bu);
  TileConvertLinearToTiledDepth(tiled.data()+1,linear.data()+1,info.format,width,height,pitch,words*4);
  TileConvertTiledToLinearDepth(back.data()+1,tiled.data()+1,info.format,width,height,pitch,words*4);
  Require(tiled.front()==guard&&tiled.back()==guard&&back.front()==guard&&back.back()==guard);
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
   Require(back[1+static_cast<size_t>(y)*pitch+x]==linear[1+static_cast<size_t>(y)*pitch+x]);
  bool rejected=false;
  try {TileConvertLinearToTiledDepth(tiled.data()+1,linear.data()+1,info.format,width,height,pitch,words*4-4);}
  catch(const std::runtime_error&) {rejected=true;}
  Require(rejected);
 }
 std::puts("PASS: R32Float depth storage footprint, bit-exact round trips and buffer boundaries");
}
