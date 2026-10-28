#include <source_location>
#include <cstdlib>
static void Require(bool ok,std::source_location at=std::source_location::current()) {if(!ok){std::fprintf(stderr,"Mip view regression line %u\n",at.line());std::exit(1);}}
int main(){
 ShaderTextureResource d{{0xc57ea100,0xc4700002,0x010dc1df,0x91b66fac,0,0x00700050,0,0}};
 Require(Prospero::NumBytesPerElement(d.Format())==8 && d.NonMsaaMipLevels()==7);
 std::array<TileSizeOffset,16> before{},after{};std::array<TilePaddedSize,16> pb{},pa{};TileSizeAlign sb{},sa{};
 ProductionTile27Layout(d.Format(),1920,1080,1920,6,27,&sb,before.data(),pb.data());
 ProductionTile27Layout(d.Format(),1920,1080,1920,d.NonMsaaMipLevels(),27,&sa,after.data(),pa.data());
 Require(sa.size==sb.size && sa.align==65536 && sb.align==65536);
 for(uint32_t l=0;l<6;l++)Require(std::memcmp(&before[l],&after[l],sizeof(TileSizeOffset))==0 && std::memcmp(&pb[l],&pa[l],sizeof(TilePaddedSize))==0);
 Require(after[6].size==30*17*8 && after[6].src_size==65536 && after[6].src_offset==0);
 Require(after[6].x+30<=pa[6].width && after[6].y+17<=pa[6].height);
 Require(after[5].x>=after[6].x+30 || after[6].x>=after[5].x+60 || after[5].y>=after[6].y+17 || after[6].y>=after[5].y+34);
 for(uint32_t l=0;l<7;l++)Require(after[l].src_offset+after[l].src_size<=sa.size);
 std::printf("PASS: captured RGBA16F1920x1080 mip6/max5, backing=0x%x unchanged, mip0..5 offsets unchanged, mip6 tail bounded and disjoint\n",sa.size);
}