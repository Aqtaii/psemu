
#include <source_location>
#include <cstdlib>
static void Require(bool ok, std::source_location at=std::source_location::current()) {
 if(!ok){std::fprintf(stderr,"Depth array regression at %u\n",at.line());std::exit(1);}
}
int main() {
 ShaderTextureResource d{{0xcdd08900,0xc0700002,0x01ffc1ff,0xd1800924,2,0,0,0}};
 VulkanImage image{VulkanImageType::DepthStencil,3,{2048,2048},2048};
 Require(d.Width5()+1==2048 && d.Height5()+1==2048 && d.Depth()==2 && d.Type()==13);
 Require(IsSupportedDepthTargetDescriptor(d,image));
 Require(IsSupportedDepthTextureEncoding(d));
 ShaderRecompiler::IR::ImageResource resource{};
 resource.dimension=ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
 for(uint32_t base=0;base<3;base++) {
  d.fields[4]=2|(base<<16);Require(IsSupportedDepthTargetDescriptor(d,image));
  Require(IsSupportedDepthTextureEncoding(d));
  auto v=ResolveTargetTextureView(resource,Prospero::ImageType::kColor2DArray,base,image.layers);
  Require(v.type==vk::ImageViewType::e2DArray && v.base_layer==base && v.layer_count==3-base);
 }
 for(uint32_t bit=0;bit<32;bit++){
  if(bit<13 || (bit>=16 && bit<29))continue;
  auto bad=d;bad.fields[4]|=1u<<bit;Require(!IsSupportedDepthTextureEncoding(bad));
 }
 for(unsigned bad=0;bad<16;bad++) {
  auto copy=d;auto owner=image;
  switch(bad){
   case 0:copy.fields[4]=1;break;
   case 1:copy.fields[4]=3;break;
   case 2:copy.fields[4]=2|(3u<<16);break;
   case 3:owner.layers=0;break;
   case 4:owner.layers=2;break;
   case 5:owner.extent.width--;break;
   case 6:owner.extent.height--;break;
   case 7:owner.guest_pitch--;break;
   case 8:owner.type=VulkanImageType::Other;break;
   case 9:copy.fields[3]|=1u<<12;break;
   case 10:copy.fields[3]|=1u<<16;break;
   case 11:copy.fields[5]|=1u<<4;break;
   case 12:copy.fields[1]|=1u<<8;break;
   case 13:copy.fields[3]|=1u<<25;break;
   case 14:copy.fields[6]|=1u<<10;break;
   case 15:copy.fields[3]=(copy.fields[3]&0x0fffffffu)|(9u<<28);break;
  }
  Require(!IsSupportedDepthTargetDescriptor(copy,owner));
 }
 for(unsigned word=5;word<8;word++){
  auto bad=d;bad.fields[word]=1;Require(!IsSupportedDepthTextureEncoding(bad));
 }
 d.fields[3]=(d.fields[3]&0x0fffffffu)|(9u<<28);d.fields[4]=0;image.layers=1;
 Require(IsSupportedDepthTargetDescriptor(d,image)&&IsSupportedDepthTextureEncoding(d));
 d.fields[4]=1;Require(!IsSupportedDepthTargetDescriptor(d,image)&&!IsSupportedDepthTextureEncoding(d));
 std::puts("PASS captured three-layer D16 descriptor, subviews, reserved bits, bounds and original single-layer path");
}
