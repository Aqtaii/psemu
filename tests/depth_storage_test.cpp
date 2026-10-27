static void Require(bool ok) {if(!ok) throw std::runtime_error("regression failed");}
int main() {
 {
  ShaderTextureResource d{};
  const uint32_t fields[]={0x5b632f00,0xc2400002,0x010dc1df,0x91b003ac,0,0x00700000,0x207b0000,0x025b96c3};
  std::memcpy(d.fields,fields,sizeof(fields));
  ShaderRecompiler::IR::ImageResource resource{ShaderRecompiler::IR::ResourceKind::StorageImage,ShaderRecompiler::Decoder::ImageDimension::Dim2D};
  Require(IsSupportedStorageTextureDescriptor(resource,d));
  Require(!IsSupportedStorageTextureEncoding(d) && IsCapturedDccStorageEncoding(d));
  Require(IsSupportedStorageSwizzle(d.Format(),d.DstSelXYZW()));
  ImageInfo packed{};packed.address=d.Base40()<<8u;packed.size=0x870000;
  packed.format=d.Format();packed.width=packed.pitch=1920;packed.height=1080;
  packed.depth=packed.levels=packed.view_levels=1;packed.type=d.Type();packed.tile=d.TileMode();
  RenderTargetInfo old{};old.address=packed.address;old.size=packed.size;
  old.width=old.pitch=1920;old.height=1080;old.bytes_per_element=4;old.tile_mode=packed.tile;
  Require(CanReconstructRenderTargetForStorage(old,true,false,true,false,true,false,packed));
  Require(ClassifyStorageSampledOverlap(packed,packed,vk::Format::eB10G11R11UfloatPack32,
   vk::Format::eB10G11R11UfloatPack32,false,false,true)==StorageSampledOverlap::CleanImage);
  Require(IsCoherentSampledStorageOwner(false,false,false,false,false));

  auto wrong=packed;wrong.size-=0x10000;
  Require(!CanReconstructRenderTargetForStorage(old,true,false,true,false,true,false,wrong));

  resource.read=true;Require(!IsSupportedStorageTextureDescriptor(resource,d));
  resource.read=false;
  for(int bad=0;bad<6;bad++){
   auto invalid=d;
   if(bad==0)invalid.fields[6]^=0x10000;
   if(bad==1)invalid.fields[3]^=0x8;
   if(bad==2)invalid.fields[5]|=1;
   if(bad==3)invalid.fields[3]|=0x1000;
   if(bad==4)invalid.fields[6]=invalid.fields[7]=0;
   if(bad==5)invalid.fields[1]^=0x100000;
   Require(!IsCapturedDccStorageEncoding(invalid));
  }
 }

 // Actual HTile allocation is adjacent to stencil; shared pages must retain ownership.
 {
  constexpr uint64_t htile=0x243c0e80000ull, bytes=0x30000;
  Require(DepthMetadataPagesOverlap(htile,bytes,htile,bytes));
  Require(DepthMetadataPagesOverlap(htile,bytes,htile+bytes-1,0x1000));
  Require(!DepthMetadataPagesOverlap(htile,bytes,htile+bytes,0x1000));
  Require(!DepthMetadataPagesOverlap(htile,bytes,htile-0x280000,0x280000));
  Require(!DepthMetadataPagesOverlap(htile,bytes,0,0));
  Require(!DepthMetadataPagesOverlap(0,0,htile,bytes));
  Require(!DepthMetadataPagesOverlap(htile,bytes,htile,0));
  // Disjoint bytes on one watched page still conflict during retirement.
  Require(DepthMetadataPagesOverlap(htile+1,1,htile+3,1));
  bool rejected=false;
  try { (void)DepthMetadataPagesOverlap(htile,bytes,UINT64_MAX-1,4); }
  catch(const std::runtime_error&) {rejected=true;}
  Require(rejected);
 }

 // Both aspects publish together; a conflict in either must leave all bytes untouched.
 for(int conflict=0;conflict<6;conflict++) {
  BufferCache cache;
  auto owner=std::make_unique<BufferCache::CachedBuffer>();owner->size=0x20000;
  cache.m_buffers.emplace(0x10000,std::move(owner));
  owner=std::make_unique<BufferCache::CachedBuffer>();owner->size=0x20000;
  cache.m_buffers.emplace(0x50000,std::move(owner));
  if(conflict==1)cache.m_memory_tracker.gpu={{0x10000,0x1000}};
  if(conflict==2)cache.m_memory_tracker.gpu={{0x50000,0x1000}};
  if(conflict==3)cache.m_gpu_modified_ranges.dirty={{0x50000,0x1000}};
  if(conflict==4)cache.m_memory_tracker.cpu={{0x50000,0x1000}};
  if(conflict==5)cache.m_memory_tracker.cpu={{0x10000,0x1000}};
  bool called=false,rejected=false;std::array<unsigned,2> pixels{11,22};
  try {cache.ReconstructImageBackingPair(0x10000,0x20000,0x50000,0x20000,[&]{called=true;pixels={33,44};});}
  catch(const std::runtime_error&){rejected=true;}
  Require(rejected==(conflict!=0));Require(called==(conflict==0));
  Require(pixels==(conflict==0?std::array<unsigned,2>{33,44}:std::array<unsigned,2>{11,22}));
  Require(cache.m_memory_tracker.published.size()==(conflict==0?2:0));
 }
 for(int invalid=0;invalid<4;invalid++) {
  BufferCache cache;bool called=false,rejected=false;
  uint64_t second=0x50000,size=0x20000;
  if(invalid==0)second=0x20000;if(invalid==1)second=0;
  if(invalid==2)size=0;if(invalid==3)second=UINT64_MAX-0x10000;
  try {cache.ReconstructImageBackingPair(0x10000,0x20000,second,size,[&]{called=true;});}
  catch(const std::runtime_error&){rejected=true;}
  Require(rejected && !called && cache.m_memory_tracker.published.empty());
 }
 // Preserve independent R8 stencil padding while reconstructing all visible pixels.
 {
  const auto format=Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt);
  constexpr uint32_t width=1920,height=1080,pitch=2048;constexpr size_t bytes=0x280000;
  std::vector<uint8_t> linear(bytes,0),tiled(bytes,0xa5),roundtrip(bytes,0);
  for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++)linear[y*pitch+x]=uint8_t(x*3+y*7);
  TileConvertLinearToTiledDepthPreservePadding(tiled.data(),linear.data(),format,width,height,pitch,bytes);
  TileConvertTiledToLinearDepth(roundtrip.data(),tiled.data(),format,width,height,pitch,bytes);
  for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++)
   Require(roundtrip[y*pitch+x]==linear[y*pitch+x]);
  std::vector<uint8_t> ones(bytes,1),mask(bytes,0);
  TileConvertLinearToTiledDepth(mask.data(),ones.data(),format,width,height,pitch,bytes);
  size_t visible=0;
  for(size_t i=0;i<bytes;i++) {visible+=mask[i]!=0;if(mask[i]==0)Require(tiled[i]==0xa5);}
  Require(visible==size_t(width)*height);
 }

 // Actual Astro descriptor: zero PerfMod is valid, as are the other hardware hints.
 {
  ShaderTextureResource d{{0xfa3e3900,0xc1600001,0x010dc1df,0x91800924,0,0,0,0}};
  for(uint32_t hint=0;hint<8;hint++) {
   d.fields[5]=hint<<20;Require(IsSupportedDepthTextureEncoding(d));
  }
  d.fields[5]=0;
  // Only PerfMod bits may vary: reject array pitch, LOD warning, corner sampling,
  // mip statistics and all other reserved bits, and retain metadata rejection.
  for(uint32_t bit=0;bit<32;bit++) {
   if(bit>=20 && bit<=22) continue;
   auto bad=d;bad.fields[5]=1u<<bit;Require(!IsSupportedDepthTextureEncoding(bad));
  }
  for(uint32_t word=4;word<8;word++) {
   if(word==5) continue;
   auto bad=d;bad.fields[word]=1;Require(!IsSupportedDepthTextureEncoding(bad));
  }
 }

 // Astro: the sampled R8 plane is adjacent to, rather than overlapping, depth.
 {
  DepthTargetInfo d{};d.address=0x25b3a3f0000ull;d.size=0x870000;
  d.stencil_address=d.address+d.size;d.stencil_size=0x280000;
  d.width=d.pitch=1920;d.height=1080;d.tile_mode=24;
  ImageInfo s{};s.address=d.stencil_address;s.size=d.stencil_size;
  s.width=1920;s.pitch=2048;s.height=1080;s.tile=24;
  s.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt);
  s.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  Require(CanRetireSampledStencil(s,false,d));
  auto depth_pitch=s;depth_pitch.pitch=d.pitch;
  Require(!CanRetireSampledStencil(depth_pitch,false,d));
  Require(!CanRetireSampledStencil(s,true,d));
  for(int i=0;i<12;i++) {
   auto bad=s;
   switch(i) {
    case 0:bad.address-=0x10000;break;case 1:bad.size-=0x10000;break;
    case 2:bad.format=22;break;case 3:bad.width--;break;
    case 4:bad.height--;break;case 5:bad.pitch--;break;
    case 6:bad.tile++;break;case 7:bad.base_level=1;break;
    case 8:bad.levels=2;break;case 9:bad.view_levels=2;break;
    case 10:bad.depth=2;break;case 11:bad.base_array=1;break;
   }
   Require(!CanRetireSampledStencil(bad,false,d));
  }
  d.layers=2;Require(!CanRetireSampledStencil(s,false,d));
  d.layers=1;d.stencil_address=0;Require(!CanRetireSampledStencil(s,false,d));
 }

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
  auto bad=sub;bad.address=target.address-0x10000;Require(accepted(bad));
  bad=sub;bad.address=target.address+target.size-0x10000;Require(accepted(bad));
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
  Require(CanReconstructRenderTargetForStorage(target,true,false,true,false,true,false,bad));
  bad.address=target.address+target.size;
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

 // Captured clean small target inside a differently shaped larger allocation.
 {
  RenderTargetInfo old{};old.address=0x1b86cb50000ull;old.size=0xc0000;
  RenderTargetInfo next{};next.address=0x1b86c5e0000ull;next.size=0xff0000;
  Require(CanRetireCleanRenderTargetForRenderTarget(old,false,false,false,true,true,next));
  for(int bad=0;bad<5;bad++)
   Require(!CanRetireCleanRenderTargetForRenderTarget(old,bad==0,bad==1,bad==2,bad!=3,bad!=4,next));
  auto cross=old;cross.address=next.address+next.size-0x10000;
  Require(!CanRetireCleanRenderTargetForRenderTarget(cross,false,false,false,true,true,next));
  cross=old;cross.address++;
  Require(!CanRetireCleanRenderTargetForRenderTarget(cross,false,false,false,true,true,next));
 }

 // Partial overlapping allocation must publish the whole old GPU owner.
 {
  auto old=rt;old.address=0x2735d470000ull;
  RenderTargetInfo next{};next.address=old.address-0xcf0000;next.size=0xff0000;
  Require(CanReconstructRenderTargetForRenderTarget(old,true,false,true,false,true,false,next));
  for(int bad=0;bad<6;bad++)
   Require(!CanReconstructRenderTargetForRenderTarget(old,bad!=0,bad==1,bad!=2,bad==3,bad!=4,bad==5,next));
  next.address=old.address+old.size;
  Require(!CanReconstructRenderTargetForRenderTarget(old,true,false,true,false,true,false,next));
 }

 // Captured 2432x1368 RGBA16F overlaps a new target but also stale buffer pages outside it.
 {
  RenderTargetInfo old{};old.address=0x2a1e6300000ull;old.size=0x1a20000;
  old.width=old.pitch=2432;old.height=1368;old.bytes_per_element=8;
  old.tile_mode=Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  RenderTargetInfo next{};next.address=old.address-0xcf0000;next.size=0xff0000;
  Require(CanReconstructRenderTargetForRenderTarget(old,true,false,true,false,true,false,next));
  Require(!CanReconstructRenderTargetForRenderTarget(old,true,false,true,false,true,true,next));
  BufferCache cache;
  const auto tail=old.address+0x1000000;
  cache.m_buffers.emplace(tail,std::make_unique<BufferCache::CachedBuffer>(BufferCache::CachedBuffer{0xa20000}));
  bool published=false;
  cache.ReconstructImageBacking(old.address,old.size,[&]{published=true;});
  Require(published && cache.m_memory_tracker.published.size()==1);
  Require(cache.m_memory_tracker.published[0]==std::pair<uint64_t,uint64_t>{old.address,old.size});
 }

 // Captured straddling RGBA8 target -> RGBA16F storage: publish the full old
 // target, including 0x2c0000 bytes beyond the new allocation's upper bound.
 {
  auto image=storage;image.address=0x183de6c0000ull;image.size=0x1a20000;
  image.width=image.pitch=2432;image.height=1368;
  image.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  image.tile=Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  RenderTargetInfo old{};old.address=image.address+0x1470000;old.size=0x870000;
  old.width=old.pitch=1920;old.height=1080;old.bytes_per_element=4;old.tile_mode=image.tile;
  Require(CanReconstructRenderTargetForStorage(old,true,false,true,false,true,false,image));
  for(int bad=0;bad<6;bad++)Require(!CanReconstructRenderTargetForStorage(old,bad!=0,bad==1,bad!=2,bad==3,bad!=4,bad==5,image));
  auto disjoint=image;disjoint.address=old.address+old.size;
  Require(!CanReconstructRenderTargetForStorage(old,true,false,true,false,true,false,disjoint));
  auto malformed=image;malformed.size-=0x10000;
  Require(!CanReconstructRenderTargetForStorage(old,true,false,true,false,true,false,malformed));
  const size_t offset=(old.address-image.address)/4,total=(offset+old.size/4);
  std::vector<uint32_t> actual(total+2,0xa5a5a5a5),expected=actual,linear(old.size/4);
  for(size_t i=0;i<linear.size();i++)linear[i]=static_cast<uint32_t>(i*17+7);
  size_t tail_pixels=0;
  for(uint32_t y=0;y<old.height;y++)for(uint32_t x=0;x<old.width;x++){
   const auto physical=((y/128)*15+x/128)*16384+Gen5RenderTargetOffsetInBlock<uint32_t>(x%128,y%128)/4;
   expected[1+offset+physical]=linear[y*old.pitch+x];
   tail_pixels+=(offset+physical)*4>=image.size;
  }
  BufferCache cache;
  cache.m_buffers.emplace(image.address+image.size,std::make_unique<BufferCache::CachedBuffer>(BufferCache::CachedBuffer{0x2c0000}));
  cache.ReconstructImageBacking(old.address,old.size,[&]{
   TileConvertLinearToTiledRenderTarget32PreservePadding(actual.data()+1+offset,linear.data(),old.width,old.height,old.pitch,old.size);
  });
  Require(tail_pixels>0 && actual==expected);
  Require(cache.m_memory_tracker.published.size()==1);
  Require(cache.m_memory_tracker.published[0]==std::pair<uint64_t,uint64_t>{old.address,old.size});
 }

 // Captured old 16-bit target contained in larger RGBA16F storage backing.
 {
  RenderTargetInfo old{};old.address=0x1e168a50000ull;old.size=0x480000;
  old.width=1920;old.height=1080;old.pitch=2048;old.bytes_per_element=2;
  old.tile_mode=Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  auto image=storage;image.address=old.address;image.size=0x1a20000;
  image.width=image.pitch=2432;image.height=1368;
  image.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);image.tile=old.tile_mode;
  Require(CanReconstructRenderTargetForStorage(old,true,false,true,false,true,false,image));
  Require(!CanReconstructRenderTargetForStorage(old,true,false,true,false,true,true,image));
  std::vector<uint16_t> linear(old.size/2),expected(linear.size()),actual(linear.size()+2);
  actual.front()=0x1234;actual.back()=0xabcd;
  for(size_t i=0;i<linear.size();i++){
   linear[i]=static_cast<uint16_t>(i*17+7);expected[i]=actual[i+1]=static_cast<uint16_t>(i*31+11);
  }
  for(uint32_t y=0;y<1080;y++)for(uint32_t x=0;x<1920;x++){
   const auto index=((y/128)*8+x/256)*32768+Gen5RenderTargetOffsetInBlock<uint16_t>(x%256,y)/2;
   expected[index]=linear[y*2048+x];
  }
  TileConvertLinearToTiledRenderTarget16PreservePadding(actual.data()+1,linear.data(),1920,1080,2048,old.size);
  Require(actual.front()==0x1234 && actual.back()==0xabcd);
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
 // Actual clean RT crosses a new depth-storage allocation by0x30000.
 {
  RenderTargetInfo target{};target.address=0x19c696f0000ull;target.size=0xc0000;target.levels=1;target.layers=1;
  ImageInfo storage{};storage.address=0x19c69780000ull;storage.size=0x3c0000;
  storage.depth=1;storage.levels=1;storage.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  auto proof=[&](bool gpu=false,bool buffer=false,bool tracker=false,bool context=true,bool coherent=true,bool buffer_gpu=false){
   return CanRetireCleanRenderTargetEdgeForStorage(target,gpu,buffer,tracker,context,coherent,buffer_gpu,storage);
  };
  Require(proof());Require(!proof(true));Require(!proof(false,true));Require(!proof(false,false,true));
  Require(!proof(false,false,false,false));Require(!proof(false,false,false,true,false));Require(!proof(false,false,false,true,true,true));
  auto initial=storage;
  storage.address=target.address-0x30000;storage.size=0x40000;Require(proof());storage=initial;
  storage.address=target.address+target.size;Require(!proof());storage=initial;
  storage.address=target.address;Require(!proof());storage=initial;
  storage.address++;Require(!proof());storage=initial;
  storage.size++;Require(!proof());storage=initial;
  storage.depth=2;Require(!proof());storage=initial;
  storage.levels=2;Require(!proof());storage=initial;
  storage.base_level=1;Require(!proof());storage=initial;
  storage.address=UINT64_MAX-0xfff;storage.size=0x2000;Require(!proof());storage=initial;
  target.layers=2;Require(!proof());target.layers=1;target.levels=2;Require(!proof());
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
 // Actual Astro sampled texture has already yielded to a buffer, then a new
 // formatted write overlaps only its first256KiB. Preserve the existing owner.
 {
  constexpr uint64_t sampled=0x2a3b9500000ull,sampled_size=0x800000;
  auto sampled_write=[&](uint64_t address,uint64_t size,bool gpu=false,bool yielded=true,bool formatted=true) {
   return ClassifyBufferImageWrite(address,size,sampled,sampled_size,BufferImageBinding::Texture,gpu,formatted,yielded);
  };
  Require(sampled_write(0x2a3b9180000ull,0x3c0000)==BufferImageWrite::InvalidateTexture);
  Require(sampled_write(sampled+sampled_size-0x40000,0x3c0000)==BufferImageWrite::InvalidateTexture);
  Require(sampled_write(0x2a3b9180000ull,0x3c0000,true)==BufferImageWrite::Unsupported);
  Require(sampled_write(0x2a3b9180000ull,0x3c0000,false,false)==BufferImageWrite::Unsupported);
  Require(sampled_write(0x2a3b9180000ull,0x3c0000,false,true,false)==BufferImageWrite::Unsupported);
  Require(sampled_write(0x2a3b9180001ull,0x3c0000)==BufferImageWrite::Unsupported);
  Require(sampled_write(0x2a3b9180000ull,0x3c0001)==BufferImageWrite::Unsupported);
  Require(sampled_write(sampled-0x10000,0x10000)==BufferImageWrite::None);
  Require(sampled_write(sampled+sampled_size,0x10000)==BufferImageWrite::None);
  // The original contained CPU-current sampled write stays supported.
  Require(sampled_write(sampled+0x10000,0x10000,false,false,false)==BufferImageWrite::InvalidateTexture);
 }
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
 // Exact captured image descriptor goes through the actual production validator.
 {
  ShaderTextureResource d{{0x8c689600,0xc1600002,0x00434077,0x91800324,0,0x00700070,0,0}};
  ShaderRecompiler::IR::ImageResource resource{ShaderRecompiler::IR::ResourceKind::StorageImage,
   ShaderRecompiler::Decoder::ImageDimension::Dim2D,false,true};
  Require(d.Format()==22 && d.Width5()==479 && d.Height5()==269 && d.MaxMip()==7);
  for(uint32_t mip=0;mip<8;++mip) {
   d.fields[3]=0x91800324|(mip<<12)|(mip<<16);
   Require(IsSupportedStorageTextureDescriptor(resource,d) && IsSupportedStorageTextureEncoding(d));
  }
  for(uint32_t flags:std::array<uint32_t,3>{0x91801324,0x91810324,0x91888324}) {
   d.fields[3]=flags;Require(!IsSupportedStorageTextureDescriptor(resource,d));
  }
  d.fields[3]=0x91811324;resource.read=true;
  Require(!IsSupportedStorageTextureDescriptor(resource,d));
 }
 // Captured ps_logo output: R32Float, RRR1, mip0 view of eight depth mips.
 Require(IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float),0x324));
 Require(!IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16Float),0x324));
 {
  ImageInfo chain{};chain.address=0x10000;chain.size=0x160000;chain.width=480;chain.height=270;
  chain.pitch=512;chain.levels=8;chain.view_levels=1;chain.depth=1;
  chain.format=Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  chain.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  chain.tile=Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  DepthStorageMipLayout plan{};Require(PlanDepthStorageMipLayout(chain,&plan));
  auto sampled=chain;sampled.view_levels=8;
  const auto classify=[&](const ImageInfo& view,bool gpu=true,bool dirty=false,bool context=true) {
   return ClassifyStorageSampledOverlap(view,chain,vk::Format::eR32Sfloat,vk::Format::eR32Sfloat,gpu,dirty,context);
  };
  Require(classify(sampled)==StorageSampledOverlap::ExactImage);
  sampled.base_level=7;sampled.view_levels=1;Require(classify(sampled)==StorageSampledOverlap::ExactImage);
  for(int field=0;field<7;++field){
   auto bad=chain;bad.view_levels=8;
   switch(field){case 0:bad.view_levels=0;break;case 1:bad.view_levels=9;break;case 2:bad.base_level=8;break;
    case 3:bad.base_level=7;bad.view_levels=2;break;case 4:++bad.pitch;break;case 5:--bad.size;break;case 6:++bad.width;break;}
   Require(classify(bad)==StorageSampledOverlap::Unsupported);
  }
  sampled=chain;sampled.view_levels=8;
  Require(classify(sampled,false)==StorageSampledOverlap::CleanImage);
  Require(classify(sampled,true,true)==StorageSampledOverlap::Unsupported);
  Require(classify(sampled,false,true)==StorageSampledOverlap::Unsupported);
  Require(IsCoherentSampledStorageOwner(false,false,false,false,false));
  Require(IsCoherentSampledStorageOwner(true,false,false,true,false));
  for(int bad=0;bad<4;bad++)Require(!IsCoherentSampledStorageOwner(false,bad==0,bad==1,bad==2,bad==3));
  Require(!IsCoherentSampledStorageOwner(true,false,false,false,false));

  Require(classify(sampled,true,false,false)==StorageSampledOverlap::Unsupported);
  const std::array<uint64_t,8> offsets{0,0xc0000,0x100000,0x110000,0x120000,0x130000,0x140000,0x150000};
  for(uint32_t i=0;i<8;++i)Require(plan[i].offset==offsets[i]);
  for(uint32_t mip=0;mip<8;++mip) {
   auto write_view=chain;write_view.base_level=mip;DepthStorageMipLayout same_plan{};
   Require(PlanDepthStorageMipLayout(write_view,&same_plan));
   for(uint32_t l=0;l<8;++l)Require(same_plan[l].offset==plan[l].offset && same_plan[l].size==plan[l].size);
  }
  for(uint32_t field=0;field<9;++field) {
   auto bad=chain;
   switch(field){case 0:--bad.size;break;case 1:++bad.pitch;break;case 2:bad.levels=0;break;
    case 3:bad.levels=10;break;case 4:bad.base_level=8;break;case 5:bad.view_levels=2;break;
    case 6:bad.depth=2;break;case 7:bad.address+=4;break;case 8:bad.width=0;break;}
   DepthStorageMipLayout untouched{};untouched[0].offset=0xabc;
   Require(!PlanDepthStorageMipLayout(bad,&untouched) && untouched[0].offset==0xabc);
  }
  std::vector<uint32_t> linear(chain.size/4+2,0xaaaaaaaa),guest(chain.size/4+2,0xdeadbeef),decoded(chain.size/4+2,0);
  for(uint32_t l=0;l<8;++l)for(uint32_t y=0;y<plan[l].height;++y)for(uint32_t x=0;x<plan[l].width;++x)
   linear[1+plan[l].offset/4+y*plan[l].pitch+x]=0x3f000000u+l*0x10000u+y*512u+x;
  TileDepthStorageMipsPreservePadding(guest.data()+1,linear.data()+1,chain);
  Require(guest.front()==0xdeadbeef && guest.back()==0xdeadbeef);
  for(uint32_t l=0;l<8;++l) {
   const auto& m=plan[l];
   TileConvertTiledToLinearDepth(decoded.data()+1+m.offset/4,guest.data()+1+m.offset/4,
     chain.format,m.width,m.height,m.pitch,m.size);
   for(uint32_t y=0;y<m.height;++y)for(uint32_t x=0;x<m.width;++x)
    Require(decoded[1+m.offset/4+y*m.pitch+x]==linear[1+m.offset/4+y*m.pitch+x]);
   std::vector<uint32_t> mask(m.size/4,0),ones(m.size/4,0xffffffff);
   TileConvertLinearToTiledDepth(mask.data(),ones.data(),chain.format,m.width,m.height,m.pitch,m.size);
   for(size_t word=0;word<mask.size();++word)if(!mask[word])Require(guest[1+m.offset/4+word]==0xdeadbeef);
  }
  // Captured transient owner straddles the requested mip chain's right edge.
  auto old=chain;old.address+=0x120000;old.size=0xc0000;old.levels=1;
  Require(CanReconstructStorageForStorage(old,true,false,true,false,true,false,chain));
  Require(CanReconstructStorageForStorage(chain,true,false,true,false,true,false,old));
  Require(old.address<chain.address+chain.size && old.address+old.size>chain.address+chain.size);
  for(int bad=0;bad<7;++bad){
   auto owner=old;if(bad==6)--owner.size;
   Require(!CanReconstructStorageForStorage(owner,bad!=0,bad==1,bad!=2,bad==3,bad!=4,bad==5,chain));
  }
  auto disjoint=old;disjoint.address=chain.address+chain.size;
  Require(!CanReconstructStorageForStorage(disjoint,true,false,true,false,true,false,chain));
  auto sampled_neighbor=old;sampled_neighbor.swizzle=0x924;
  Require(CanReconstructStorageForSampled(chain,true,false,true,false,true,false,sampled_neighbor));
  for(int bad=0;bad<8;++bad) {
   auto view=sampled_neighbor;
   if(bad==6)view.view_levels=0;
   if(bad==7)view.base_level=view.levels;
   Require(!CanReconstructStorageForSampled(chain,bad!=0,bad==1,bad!=2,bad==3,bad!=4,bad==5,view));
  }
  auto sampled_chain=chain;sampled_chain.address+=0x10000;sampled_chain.view_levels=8;
  Require(CanReconstructStorageForSampled(chain,true,false,true,false,true,false,sampled_chain));
  sampled_chain.base_level=7;sampled_chain.view_levels=2;
  Require(!CanReconstructStorageForSampled(chain,true,false,true,false,true,false,sampled_chain));
  std::vector<uint8_t> whole(0x1e0000+2,0xa5),old_linear(old.size,0);
  for(uint32_t y=0;y<old.height;++y)for(uint32_t x=0;x<old.width;++x){
   const uint32_t value=0x3e800000u+y*512+x;std::memcpy(old_linear.data()+(y*old.pitch+x)*4,&value,4);
  }
  TileDepthStorageMipsPreservePadding(whole.data()+1+0x120000,old_linear.data(),old);
  for(size_t i=0;i<0x120000+1;++i)Require(whole[i]==0xa5);
  Require(whole.back()==0xa5);
  std::vector<uint8_t> old_decoded(old.size,0);
  TileConvertTiledToLinearDepth(old_decoded.data(),whole.data()+1+0x120000,old.format,old.width,old.height,old.pitch,old.size);
  for(uint32_t y=0;y<old.height;++y)for(uint32_t x=0;x<old.width;++x)
   Require(!std::memcmp(old_decoded.data()+(y*old.pitch+x)*4,old_linear.data()+(y*old.pitch+x)*4,4));
  // Publish the newer full eight-mip owner, then sample the old straddling view.
  // Its bytes beyond the published allocation must retain the earlier owner data.
  const std::vector<uint8_t> outside(whole.begin()+1+chain.size,whole.end());
  TileDepthStorageMipsPreservePadding(whole.data()+1,linear.data()+1,chain);
  Require(std::equal(outside.begin(),outside.end(),whole.begin()+1+chain.size));
  Require(whole.front()==0xa5 && whole.back()==0xa5);
  TileConvertTiledToLinearDepth(old_decoded.data(),whole.data()+1+0x120000,old.format,old.width,old.height,old.pitch,old.size);
  // Both views begin on the same depth64K block as backing mip4.
  for(uint32_t y=0;y<plan[4].height;++y)for(uint32_t x=0;x<plan[4].width;++x) {
   uint32_t pixel=0;std::memcpy(&pixel,old_decoded.data()+(y*old.pitch+x)*4,4);
   Require(pixel==linear[1+plan[4].offset/4+y*plan[4].pitch+x]);
  }
  const auto before=guest;
  linear[1]=0x3e800000; // A mip0-only edit must retain all other mip bytes.
  TileDepthStorageMipsPreservePadding(guest.data()+1,linear.data()+1,chain);
  Require(std::equal(guest.begin()+1+plan[1].offset/4,guest.end(),before.begin()+1+plan[1].offset/4));
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
 std::puts("PASS: R32Float single/eight-mip depth storage, RRR1, bit-exact round trips, untouched mips/padding and buffer boundaries");
}
