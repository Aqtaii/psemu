static void Require(bool ok) {if(!ok) throw std::runtime_error("regression failed");}
int main() {
 Require(IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16Float),0x22c));
 Require(!IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16Float),DstSel(5,4,0,1)));
 Require(!IsSupportedStorageSwizzle(Prospero::GpuEnumValue(Prospero::BufferFormat::k16Float),0x22c));
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
