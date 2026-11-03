#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
int main(){try{
 ShaderTextureResource d{};const std::array<uint32_t,8> raw{
  0x100000u,0xc0500000u,0x010dc1dfu,0x91800924u,0,0,0,0};
 std::copy(raw.begin(),raw.end(),d.fields);
 CHECK(d.Format()==5 && uint32_t(d.Width5())+1==1920 && uint32_t(d.Height5())+1==1080);
 ShaderRecompiler::IR::ImageResource r;r.kind=ShaderRecompiler::IR::ResourceKind::ImageUint;
 r.dimension=ShaderRecompiler::Decoder::ImageDimension::Dim2D;r.read=true;
 VulkanImage image{VulkanImageType::DepthStencil,vk::Format::eD32SfloatS8Uint,1,1,{1920,1080},1920};
 CHECK(IsSupportedStencilTargetBinding(r,d,image,vk::Format::eR8Uint));
 for(int bad=0;bad<24;++bad){
  auto resource=r;auto descriptor=d;auto owner=image;auto format=vk::Format::eR8Uint;
  switch(bad){
   case 0:resource.kind=ShaderRecompiler::IR::ResourceKind::Image;break;
   case 1:resource.kind=ShaderRecompiler::IR::ResourceKind::StorageImageUint;break;
   case 2:resource.dimension=ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;break;
   case 3:resource.mip_mode=static_cast<ShaderRecompiler::IR::ImageMipMode>(1);break;
   case 4:resource.read=false;break;case 5:resource.written=true;break;
   case 6:resource.atomic=true;break;case 7:resource.depth_compare=true;break;
   case 8:owner.type=VulkanImageType::Other;break;case 9:owner.format=vk::Format::eD32Sfloat;break;
   case 10:owner.layers=2;break;case 11:owner.mip_levels=2;break;
   case 12:format=vk::Format::eR8Unorm;break;case 13:descriptor.fields[1]^=1u<<20;break;
   case 14:descriptor.fields[3]=(descriptor.fields[3]&~0xfffu)|DstSel(4,5,6,7);break;
   case 15:descriptor.fields[3]^=1u<<28;break;case 16:descriptor.fields[3]^=1u<<20;break;
   case 17:owner.extent.width--;break;case 18:owner.extent.height--;break;
   case 19:descriptor.fields[4]=1;break;case 20:descriptor.fields[3]|=1u<<12;break;
   case 21:descriptor.fields[6]=1;break;case 22:descriptor.fields[7]=1;break;
   case 23:format=vk::Format::eR8Sint;break;
  }
  CHECK(!IsSupportedStencilTargetBinding(resource,descriptor,owner,format));
 }
 GraphicContext ctx;DepthStencilVulkanImage native;TextureCache cache;
 native.type=VulkanImageType::DepthStencil;native.format=vk::Format::eD32SfloatS8Uint;
 native.layers=native.mip_levels=1;native.image=reinterpret_cast<VkImage>(uintptr_t(1));
 cache.GetStencilTargetSampledView(&ctx,&native,DstSel(4,4,4,4));
 CHECK(cache.calls==1 && cache.info.format==native.format && cache.info.aspect==vk::ImageAspectFlagBits::eStencil);
 CHECK(cache.info.type==vk::ImageViewType::e2D && cache.info.usage==vk::ImageUsageFlagBits::eSampled);
 CHECK(cache.info.base_level==0 && cache.info.level_count==1 && cache.info.base_layer==0 && cache.info.layer_count==1);
 CHECK(cache.info.swizzle==DstSel(4,4,4,4));
 for(int bad=0;bad<5;++bad){
  auto test=native;uint32_t swizzle=DstSel(4,4,4,4);
  switch(bad){case 0:test.image=nullptr;break;case 1:test.format=vk::Format::eD32Sfloat;break;
   case 2:test.layers=2;break;case 3:test.mip_levels=2;break;case 4:swizzle=DstSel(4,5,6,7);break;}
  bool rejected=false;try{cache.GetStencilTargetSampledView(&ctx,&test,swizzle);}catch(const std::runtime_error&){rejected=true;}
  CHECK(rejected && cache.calls==1);
 }
 puts("PASS extracted stencil binding/view: captured UInt1920x1080, native S8 aspect/RRRR, 24 descriptor and 5 view rejections");
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL stencil binding: %s\n",e.what());return 1;}}
