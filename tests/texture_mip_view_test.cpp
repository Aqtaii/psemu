#include <source_location>
#include <cstdlib>
static void Require(bool ok,std::source_location at=std::source_location::current()) {if(!ok){std::fprintf(stderr,"Mip view regression line %u\n",at.line());std::exit(1);}}
int main(){
 ShaderTextureResource d{{0xc57ea100,0xc4700002,0x010dc1df,0x91b66fac,0,0x00700050,0,0}};
 Require(Prospero::NumBytesPerElement(d.Format())==8 && d.NonMsaaMipLevels()==7);
 std::array<TileSizeOffset,16> before{},after{};std::array<TilePaddedSize,16> pb{},pa{};TileSizeAlign sb{},sa{};
 TileGetTextureSize(d.Format(),1920,1080,1920,6,27,&sb,before.data(),pb.data());
 TileGetTextureSize(d.Format(),1920,1080,1920,d.NonMsaaMipLevels(),27,&sa,after.data(),pa.data());
 Require(sa.size==sb.size && sa.align==65536 && sb.align==65536);
 for(uint32_t l=0;l<6;l++)Require(std::memcmp(&before[l],&after[l],sizeof(TileSizeOffset))==0 && std::memcmp(&pb[l],&pa[l],sizeof(TilePaddedSize))==0);
 Require(after[6].size==30*17*8 && after[6].src_size==65536 && after[6].src_offset==0);
 Require(after[6].x+30<=pa[6].width && after[6].y+17<=pa[6].height);
 Require(after[5].x>=after[6].x+30 || after[6].x>=after[5].x+60 || after[5].y>=after[6].y+17 || after[6].y>=after[5].y+34);
 for(uint32_t l=0;l<7;l++)Require(after[l].src_offset+after[l].src_size<=sa.size);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1920,6,sa.size)==11);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1920,7,sa.size)==11);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1920,11,sa.size)==11);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1920,1,sa.size)==1);
 Require(TileGetEquivalentRenderTargetMipLevels(22,1920,1080,1920,6,sa.size)==6);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1919,6,sa.size)==6);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1920,6,sa.size-65536)==6);
 Require(TileGetEquivalentRenderTargetMipLevels(d.Format(),1920,1080,1920,12,sa.size)==12);
 ImageInfo request{};request.address=0x100000;request.size=sa.size;request.format=d.Format();
 request.width=request.pitch=1920;request.height=1080;request.depth=1;request.levels=6;request.view_levels=1;
 request.tile=27;request.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);request.swizzle=0xfac;
 auto expected=request;expected.levels=11;
 auto canonical=CanonicalTextureMipBacking(request);Require(std::memcmp(&canonical,&expected,sizeof(ImageInfo))==0);
 request.levels=7;request.base_level=6;expected=request;expected.levels=11;
 canonical=CanonicalTextureMipBacking(request);Require(std::memcmp(&canonical,&expected,sizeof(ImageInfo))==0);
 request.base_level=0;request.view_levels=7;expected=request;expected.levels=11;
 canonical=CanonicalTextureMipBacking(request);Require(std::memcmp(&canonical,&expected,sizeof(ImageInfo))==0);
 for(int field=0;field<7;field++){
  auto bad=request;if(field==0)bad.type=Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray);
  if(field==1)bad.depth=2;if(field==2)bad.base_array=1;if(field==3)bad.base_level=7;
  if(field==4)bad.view_levels=0;if(field==5)bad.tile=24;if(field==6)bad.size-=65536;
  const auto unchanged=CanonicalTextureMipBacking(bad);Require(unchanged.levels==bad.levels);
 }
 std::printf("PASS: captured RGBA16F1920x1080 mip6/max5, backing=0x%x unchanged, mip0..5 offsets unchanged, mip6 tail bounded and disjoint\n",sa.size);
}