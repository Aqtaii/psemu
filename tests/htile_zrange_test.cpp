#define NOMINMAX
#include <windows.h>
#include "graphics/host_gpu/renderer/htileZRange.h"
#include "graphics/shader/recompiler/InstructionCodeHash.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <limits>
using namespace Libs::Graphics;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
int main() {try {
    const std::array<uint32_t,1> text{0x74736574u}; // "test", little endian.
    CHECK(ShaderRecompiler::InstructionCodeHash(text)==0xf9e6e6ef197c2b25ull);
    HtileZRangeLayout layout;
    CHECK(MakeHtileZRangeLayout(1920,1080,2,0x30000,0xc0000,&layout));
    CHECK(layout.tiles_x==240 && layout.tiles_y==135);
    CHECK(HtileZRangeByteOffset(layout,0,0)==0);
    CHECK(HtileZRangeByteOffset(layout,1,0)==0x104);
    CHECK(HtileZRangeByteOffset(layout,2,0)==0x200);
    CHECK(HtileZRangeByteOffset(layout,0,1)==0x100);
    CHECK(HtileZRangeByteOffset(layout,1,1)==4);
    CHECK(HtileZRangeByteOffset(layout,128,0)==0x8000);
    CHECK(HtileZRangeByteOffset(layout,0,64)==0x10000);
    std::vector<bool> seen(0x30000/4,false);
    for(uint32_t y=0;y<192;++y)for(uint32_t x=0;x<256;++x) {
        const auto offset=HtileZRangeByteOffset(layout,x,y);
        CHECK(offset<0x30000 && offset%4==0 && !seen[offset/4]);
        seen[offset/4]=true;
    }
    for(bool value:seen)CHECK(value);
    CHECK(HtileZRangeByteOffset(layout,256,0)==UINT64_MAX);
    CHECK(HtileZRangeByteOffset(layout,0,192)==UINT64_MAX);
    std::vector<float> depth(1920*1080);
    for(uint32_t y=0;y<1080;++y)for(uint32_t x=0;x<1920;++x)
        depth[size_t(y)*1920+x]=float((x/8*7+y/8*11)%101)/101.f+float((x+y)%7)*.0001f;
    depth[0]=0;depth[1]=1;
    const auto original=depth;
    std::vector<uint32_t> snapshot;
    CHECK(BuildHtileZRangeSnapshot(depth,layout,&snapshot));
    CHECK(depth==original && snapshot.size()==0xc0000/4);
    for(uint32_t y=0;y<135;++y)for(uint32_t x=0;x<240;++x) {
        const auto word=snapshot[HtileZRangeByteOffset(layout,x,y)/4];
        CHECK((word&15)==15);
        const double low=double((word>>4)&16383)/16383.0,high=double(word>>18)/16383.0;
        for(uint32_t dy=0;dy<8;++dy)for(uint32_t dx=0;dx<8;++dx) {
            const auto z=depth[size_t(y*8+dy)*1920+x*8+dx];
            CHECK(low<=z && high>=z);
            if(x || y) CHECK(high-low<.002);
        }
    }
    CHECK(snapshot[0]==HtileExpandedFullRange); // First tile contains both0 and1.
    for(uint32_t y=0;y<192;++y)for(uint32_t x=0;x<256;++x)
        if(x>=240 || y>=135)CHECK(snapshot[HtileZRangeByteOffset(layout,x,y)/4]==HtileExpandedFullRange);
    for(size_t i=0x30000/4;i<snapshot.size();++i)CHECK(snapshot[i]==HtileExpandedFullRange);
    const auto saved=snapshot;const auto saved_layout=layout;
    for(float invalid:{-1.f,2.f,std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
        depth[47]=invalid;
        CHECK(!BuildHtileZRangeSnapshot(depth,layout,&snapshot) && snapshot==saved);
    }
    CHECK(!BuildHtileZRangeSnapshot({},layout,&snapshot) && snapshot==saved);
    for(uint32_t pitch:{0u,1u,3u})
        CHECK(!MakeHtileZRangeLayout(1920,1080,pitch,0x30000,0xc0000,&layout));
    CHECK(layout.width==saved_layout.width && layout.metadata_bytes==saved_layout.metadata_bytes);
    CHECK(!MakeHtileZRangeLayout(1920,1080,2,0x30001,0xc0000,&layout));
    CHECK(!MakeHtileZRangeLayout(1920,1080,2,0x30000,0x2ffff,&layout));
    CHECK(!MakeHtileZRangeLayout(1920,1080,2,0x30000,MaxHtileZRangeSnapshotBytes+4,&layout));
    CHECK(!MakeHtileZRangeLayout(1921,1080,2,0x30000,0xc0000,&layout));
    CHECK(!MakeHtileZRangeLayout(1920,1081,2,0x30000,0xc0000,&layout));
    CHECK(MakeHtileZRangeLayout(8,8,1,32768,32768,&layout));
    for(float value:{0.f,1.f}) {
        depth.assign(64,value);
        CHECK(BuildHtileZRangeSnapshot(depth,layout,&snapshot));
        const uint32_t bits=value==0 ? 0 : 16383;
        CHECK(snapshot[0]==((bits<<18)|(bits<<4)|15u));
    }
    puts("PASS HTile Z-range: complete swizzle bijection/golden addresses, 2073600 depth samples, conservative bounds, padding/range/nonfinite guards and immutable input");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL HTile Z-range: %s\n",e.what());return 1;}}
