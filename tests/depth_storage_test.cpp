static void Require(bool ok) {if(!ok) throw std::runtime_error("regression failed");}
int main() {
 ImageInfo storage{};storage.address=0x247db190000;storage.size=0x280000;
 storage.width=960;storage.height=540;storage.pitch=1024;storage.depth=1;
 storage.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
 storage.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
 storage.tile=Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
 storage.levels=storage.view_levels=1;
 RenderTargetInfo rt{};rt.address=storage.address;rt.size=storage.size;rt.width=960;rt.height=540;
 rt.pitch=1024;rt.bytes_per_element=4;rt.tile_mode=Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB);
 Require(CanReconstructRenderTargetForStorage(rt,true,false,true,false,true,false,storage));
 for(int bad=0;bad<6;bad++)Require(!CanReconstructRenderTargetForStorage(rt,bad!=0,bad==1,bad!=2,bad==3,bad!=4,bad==5,storage));
 auto wrong=storage;wrong.pitch=960;Require(!CanReconstructRenderTargetForStorage(rt,true,false,true,false,true,false,wrong));
 std::vector<uint32_t> src(rt.size/4,0x12345678),tiled(src.size(),0),keep(src.size(),0xa5a5a5a5),mask(src.size(),0),ones(src.size(),1);
 TileConvertLinearToTiledStandard64KB32(tiled.data(),src.data(),960,540,1024,rt.size);
 TileConvertLinearToTiledStandard64KB32(mask.data(),ones.data(),960,540,1024,rt.size);
 TileConvertLinearToTiledStandard64KB32PreservePadding(keep.data(),src.data(),960,540,1024,rt.size);
 size_t pixels=0;for(size_t i=0;i<keep.size();i++){Require(keep[i]==(mask[i]?tiled[i]:0xa5a5a5a5));pixels+=mask[i]!=0;}
 Require(pixels==960u*540u);
 rt.tile_mode=Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
 Require(CanReconstructRenderTargetForStorage(rt,true,false,true,false,true,false,storage));
 std::fill(mask.begin(),mask.end(),0);
 std::fill(keep.begin(),keep.end(),0xa5a5a5a5);
 for(size_t i=0;i<src.size();i++)src[i]=static_cast<uint32_t>(i*17+7);
 TileConvertLinearToTiledRenderTarget32PreservePadding(mask.data(),ones.data(),960,540,1024,rt.size);
 TileConvertLinearToTiledRenderTarget32PreservePadding(keep.data(),src.data(),960,540,1024,rt.size);
 pixels=0;for(size_t i=0;i<keep.size();i++){if(!mask[i])Require(keep[i]==0xa5a5a5a5);pixels+=mask[i]!=0;}
 Require(pixels==960u*540u);
 for(uint32_t y=0;y<540;y++)for(uint32_t x=0;x<960;x++){
  const uint64_t block=((y/128)*8+x/128)*16384;
  const auto index=block+Gen5RenderTargetOffsetInBlock<uint32_t>(x%128,y%128)/4;
  Require(keep[index]==src[y*1024+x]);
 }


 // Captured storage subrange requires reconstruction of the entire native target.
 {
  RenderTargetInfo target{};target.address=0x1a4db0a0000ull;target.size=0x870000;
  target.width=1920;target.height=1080;target.pitch=1920;target.bytes_per_element=4;
  target.tile_mode=Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  auto sub=storage;sub.address=target.address+0x240000;sub.size=0xc0000;
  sub.width=480;sub.height=270;sub.pitch=512;
  auto accepted=[&](const ImageInfo& image) {
   return CanReconstructRenderTargetForStorage(target,true,false,true,false,true,false,image);
  };
  Require(accepted(sub));
  auto bad=sub;bad.address=target.address-0x10000;Require(!accepted(bad));
  bad=sub;bad.address=target.address+target.size-0x10000;Require(!accepted(bad));
  bad=sub;bad.address++;Require(!accepted(bad));
  bad=sub;bad.size=0;Require(!accepted(bad));
  bad=sub;bad.address=UINT64_MAX-0xffff;Require(!accepted(bad));
  Require(!CanReconstructRenderTargetForStorage(target,true,false,true,false,true,true,sub));
  auto overflow=target;overflow.address=UINT64_MAX-0xffff;
  Require(!CanReconstructRenderTargetForStorage(overflow,true,false,true,false,true,false,sub));
  std::vector<uint32_t> linear(target.size/4),expected(linear.size()),actual(linear.size()+2);
  actual.front()=0x10203040;actual.back()=0x50607080;
  for(size_t i=0;i<linear.size();i++){
   linear[i]=static_cast<uint32_t>(i*17+7);
   expected[i]=actual[i+1]=static_cast<uint32_t>(i*31+0xa5a5);
  }
  size_t outside_pixels=0;
  for(uint32_t y=0;y<target.height;y++)for(uint32_t x=0;x<target.width;x++){
   const uint64_t block=((y/128)*15+x/128)*16384;
   const auto index=block+Gen5RenderTargetOffsetInBlock<uint32_t>(x%128,y%128)/4;
   expected[index]=linear[y*target.pitch+x];
   outside_pixels+=(index*4<0x240000 || index*4>=0x300000);
  }
  TileConvertLinearToTiledRenderTarget32PreservePadding(actual.data()+1,linear.data(),
    target.width,target.height,target.pitch,target.size);
  Require(outside_pixels>0);
  Require(actual.front()==0x10203040 && actual.back()==0x50607080);
  for(size_t i=0;i<expected.size();i++)Require(actual[i+1]==expected[i]);
 }

 // Captured RGBA16F: 128x64 blocks; absolute Y includes the block-row bank XOR.
 {
  auto image=storage;image.address=0x28cb7980000ull;image.size=0xff0000;
  image.width=1920;image.height=1080;image.pitch=1920;
  image.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  image.tile=Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  RenderTargetInfo target{};target.address=image.address;target.size=image.size;
  target.width=image.width;target.height=image.height;target.pitch=image.pitch;
  target.bytes_per_element=8;target.tile_mode=image.tile;
  Require(CanReconstructRenderTargetForStorage(target,true,false,true,false,true,false,image));
  auto bad=image;bad.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  Require(!CanReconstructRenderTargetForStorage(target,true,false,true,false,true,false,bad));
  bad=image;bad.address+=0x10000;
  Require(!CanReconstructRenderTargetForStorage(target,true,false,true,false,true,false,bad));
  std::vector<uint64_t> linear(image.size/8),expected(linear.size()),actual(linear.size()+2);
  actual.front()=0x1122334455667788ull;actual.back()=0x8877665544332211ull;
  for(size_t i=0;i<linear.size();i++){
   linear[i]=0xabcdef0100000000ull+i*17;expected[i]=actual[i+1]=0x1234567800000000ull+i*31;
  }
  for(uint32_t y=0;y<1080;y++)for(uint32_t x=0;x<1920;x++){
   const auto index=((y/64)*15+x/128)*8192+Gen5RenderTargetOffsetInBlock<uint64_t>(x%128,y)/8;
   expected[index]=linear[y*1920+x];
  }
  TileConvertLinearToTiledRenderTarget64PreservePadding(actual.data()+1,linear.data(),1920,1080,1920,image.size);
  Require(actual.front()==0x1122334455667788ull && actual.back()==0x8877665544332211ull);
  for(size_t i=0;i<expected.size();i++)Require(actual[i+1]==expected[i]);
 }

 Require(!CanMergeBufferCacheQueueMask(0x110,4));
 Require(CanMergeBufferCacheAfterDrain(0x110,4,0x110,true));
 Require(!CanMergeBufferCacheAfterDrain(0x110,4,0x10,true));
 Require(!CanMergeBufferCacheAfterDrain(0x110,4,0x100,true));
 Require(!CanMergeBufferCacheAfterDrain(0x110,4,0x110,false));
 Require(!CanMergeBufferCacheAfterDrain(0x110,64,0x110,true));
 Require(!CanMergeBufferCacheAfterDrain(0x310,4,0x110,true));

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
 // Captured R32Float depth-storage image fully inside a larger render target.
 {
  ImageInfo storage{};
  storage.address=0x1d90e540000ull;storage.size=0xc0000;
  storage.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  storage.width=480;storage.height=270;storage.pitch=512;
  storage.depth=1;storage.levels=1;storage.view_levels=1;
  storage.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  storage.tile=Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  RenderTargetInfo target{};
  target.address=0x1d90e300000ull;target.size=0xff0000;target.levels=1;target.layers=1;
  auto proof=[&](bool gpu=true,bool yielded=false,bool dirty=false,bool context=true,
                bool coherent=true,bool buffers=false) {
   return CanReconstructStorageForRenderTarget(storage,gpu,yielded,dirty,context,coherent,buffers,target);
  };
  Require(proof());
  Require(!proof(false));Require(!proof(true,true));Require(!proof(true,false,true));
  Require(!proof(true,false,false,false));Require(!proof(true,false,false,true,false));
  Require(!proof(true,false,false,true,true,true));
  auto captured=storage;
  storage.address=target.address-0x1000;Require(!proof());storage=captured;
  storage.size=target.size;Require(!proof());storage=captured;
  storage.base_level=1;Require(!proof());storage=captured;
  storage.depth=2;Require(!proof());storage=captured;
  storage.address++;Require(!proof());storage=captured;
  target.layers=2;Require(!proof());
 }
 // Captured clean 0x480000 target becomes a larger 0x1a20000 storage allocation.
 {
  RenderTargetInfo target{};
  target.address=0x1b0e61c0000ull;target.size=0x480000;target.levels=1;target.layers=1;
  ImageInfo storage{};storage.address=target.address;storage.size=0x1a20000;
  auto proof=[&](bool gpu=false,bool yielded=false,bool tracked_gpu=false,
                bool context=true,bool coherent=true) {
   return CanRetireCleanRenderTargetForStorage(target,gpu,yielded,tracked_gpu,context,coherent,storage);
  };
  Require(proof());
  Require(!proof(true));Require(!proof(false,true));Require(!proof(false,false,true));
  Require(!proof(false,false,false,false));Require(!proof(false,false,false,true,false));
  auto captured=storage;
  storage.size=0x470000;Require(!proof());storage=captured;
  storage.address+=0x1000;Require(!proof());storage=captured;
  storage.address++;Require(!proof());storage=captured;
  storage.size=0;Require(!proof());storage=captured;
  storage.address=UINT64_MAX-0xfff;storage.size=0x2000;Require(!proof());storage=captured;
  target.layers=2;Require(!proof());
 }
 // Production fragment refresh: partial native owners, cache holes and exterior bytes.
 for(int conflict=0;conflict<4;conflict++) {
  BufferCache cache;GraphicContext context;
  Libs::LibKernel::Memory::fixture_base=0x10000;
  auto& guest=Libs::LibKernel::Memory::fixture_backing;guest.resize(0x80000);
  for(size_t i=0;i<guest.size();i++)guest[i]=static_cast<uint8_t>(i*13+7);
  for(uint64_t address:{uint64_t{0x10000},uint64_t{0x60000}}) {
   auto cached=std::make_unique<BufferCache::CachedBuffer>();
   cached->size=0x30000;cached->ctx=&context;
   cached->buffer=std::make_shared<VulkanBuffer>();
   cached->buffer->buffer_size=0x30000;cached->buffer->bytes.assign(0x30000,0xaa);
   cache.m_buffers.emplace(address,std::move(cached));
  }
  cache.m_memory_tracker.cpu={{0x10000,0x30000},{0x40000,0x20000},{0x60000,0x30000}};
  cache.m_memory_tracker.gpu={{0x90000,0x10000}}; // exterior GPU data survives
  if(conflict==1)cache.m_memory_tracker.gpu.emplace_back(0x60000,0x1000);
  if(conflict==2)cache.m_gpu_modified_ranges.dirty={{0x60000,0x1000}};
  if(conflict==3)cache.m_buffers.at(0x60000)->buffer->buffer=nullptr;
  Transfer::fixture_uploads=0;
  bool rejected=false;
  try {cache.RefreshCpuImageFragmentsLocked(0x20000,0x50000);}
  catch(const std::runtime_error&){rejected=true;}
  Require(rejected==(conflict!=0));
  Require(Transfer::fixture_uploads==(rejected?0u:2u));
  for(const auto& [address,cached]:cache.m_buffers) {
   for(size_t i=0;i<cached->buffer->bytes.size();i++) {
    auto absolute=address+i;
    bool replaced=!rejected && absolute>=0x20000 && absolute<0x70000;
    Require(cached->buffer->bytes[i]==(replaced?guest[absolute-0x10000]:uint8_t{0xaa}));
   }
  }
  if(!rejected) {
   Require(!cache.IsCachedRegionCpuModified(0x20000,0x50000));
   Require(cache.m_memory_tracker.IsRegionCpuModified(0x40000,0x20000));
   Require(cache.IsCachedRegionCpuModified(0x10000,0x10000));
   Require(cache.IsCachedRegionCpuModified(0x70000,0x20000));
   Require(!cache.IsCachedRegionCpuModified(0x40000,0x20000));
   cache.RefreshCpuImageFragmentsLocked(0x20000,0x50000);
   Require(Transfer::fixture_uploads==2); // clean fragments must not upload again
  }
  Require(cache.m_memory_tracker.gpu.front()==std::pair<uint64_t,uint64_t>{0x90000,0x10000});
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
 for(auto extent:std::array<std::array<uint32_t,2>,4>{{{480,270},{960,540},{129,131},{1,1}}}) {
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
  // Reinterpreting the allocation must preserve padding not represented by Vulkan pixels.
  std::vector<uint32_t> preserved(words+2,guard),mask(words,0);
  TileConvertLinearToTiledDepthPreservePadding(preserved.data()+1,linear.data()+1,info.format,width,height,pitch,words*4);
  std::vector<uint32_t> ones(words,1);
  TileConvertLinearToTiledDepth(mask.data(),ones.data(),info.format,width,height,pitch,words*4);
  size_t replaced=0;
  for(size_t i=0;i<words;i++) {
   if(mask[i]) {Require(preserved[i+1]==tiled[i+1]);replaced++;}
   else Require(preserved[i+1]==guard);
  }
  Require(replaced==static_cast<size_t>(width)*height);
  Require(preserved.front()==guard && preserved.back()==guard);
  bool rejected=false;
  try {TileConvertLinearToTiledDepth(tiled.data()+1,linear.data()+1,info.format,width,height,pitch,words*4-4);}
  catch(const std::runtime_error&) {rejected=true;}
  Require(rejected);
 }
 std::puts("PASS: R32Float depth storage footprint, bit-exact round trips and buffer boundaries");
}
