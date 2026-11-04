#include "compute_probe_device.inc"
#include "graphics/host_gpu/renderer/computeBufferProbe.h"
#include "graphics/host_gpu/renderer/storageBufferBounce.h"
#include "graphics/host_gpu/renderer/sceneProbeWindow.h"
#include <limits>
using namespace Libs::Graphics;
struct ProbeDispatch {
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier=::vkCmdPipelineBarrier;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer=::vkCmdCopyBuffer;
};

static void Bounds() {
    SceneProbeSubmitWindow window;
    CHECK(window.Include(0));CHECK(window.Include(0));CHECK(window.Include(8749));
    CHECK(window.Include(0));CHECK(window.Include(8750));CHECK(window.Include(0));
    CHECK(!window.Include(8751));CHECK(!window.Include(0));
    SceneProbeSubmitWindow extended;
    CHECK(!extended.Include(8749,0));CHECK(!extended.Include(8749,65));
    CHECK(extended.Include(0,8));
    for(uint64_t submit=8749;submit<8757;++submit) {
        CHECK(extended.Include(submit,8));CHECK(extended.Include(0,8));
    }
    CHECK(!extended.Include(8757,8));CHECK(!extended.Include(0,8));
    CHECK(ScenePresentConstantProbeBytes(256,1024,2048)==256);
    CHECK(ScenePresentConstantProbeBytes(256,160,416)==160);
    CHECK(ScenePresentConstantProbeBytes(256,160,412)==156);
    CHECK(ScenePresentConstantProbeBytes(257,160,1024)==0);
    CHECK(ScenePresentConstantProbeBytes(512,160,256)==0);
    CHECK(ScenePresentConstantProbeBytes(UINT64_MAX-3,256,UINT64_MAX)==0);
    uint64_t offset=0;
    constexpr uint64_t bytes=12288ull*128;
    CHECK(AstroProducerProbeOffset(256,bytes,256+bytes,128,12288,12287,&offset));
    CHECK(offset==256+bytes-128);
    CHECK(!AstroProducerProbeOffset(256,bytes-1,256+bytes,128,12288,12287,&offset));
    CHECK(!AstroProducerProbeOffset(256,bytes,256+bytes,128,12288,12288,&offset));
    CHECK(!AstroProducerProbeOffset(256,bytes,256+bytes,64,12288,0,&offset));
    CHECK(!AstroProducerProbeOffset(256,bytes,256+bytes,128,12287,0,&offset));
    CHECK(!AstroProducerProbeOffset(257,bytes,257+bytes,128,12288,0,&offset));
    CHECK(!AstroProducerProbeOffset(256,bytes,255+bytes,128,12288,0,&offset));
    CHECK(!AstroProducerProbeOffset(UINT64_MAX-3,128,UINT64_MAX,128,12288,0,&offset));
    CHECK(!AstroProducerProbeOffset(256,bytes,256+bytes,128,12288,0,nullptr));
    CHECK(AstroProducerProbeKind(0x25ea92806a8f33d1ull,659)==0);
    CHECK(AstroProducerProbeKind(0x1cbf799b3cec5a07ull,5798)==1);
    CHECK(AstroProducerProbeKind(0x25ea92806a8f33d1ull,660)==-1);
    CHECK(AstroProducerProbeKind(0,659)==-1);
    uint32_t samples=0;
    for(uint32_t i=0;i<16384;++i)samples+=AstroProducerProbeOrdinal(i);
    CHECK(samples==139);
    CHECK(AstroProducerInputProbeOffset(256,432ull*2048,256+432ull*2048,432,2048,4,0x7fe,&offset));
    CHECK(offset==256+432ull*0x7fe);
    CHECK(AstroProducerInputProbeOffset(256,276ull*16384,256+276ull*16384,276,16384,3,0x3a00,&offset));
    CHECK(!AstroProducerInputProbeOffset(256,432ull*2048,256+432ull*2048,432,2048,3,0x7fe,&offset));
    CHECK(!AstroProducerInputProbeOffset(256,432ull*2048-433,256+432ull*2048,432,2048,4,0x7fe,&offset));
    std::printf("PASS producer probe descriptor/allocation bounds, overflow, shader identity and139-dispatch budget\n");
}
static void Gpu(const char* shader,uint32_t index) {
    Device device(true,shader,true);
    constexpr uint64_t bytes=12288ull*128;
    auto source=device.Make(bytes,0xa5);
    auto before=device.Make(256,0xcd),after=device.Make(256,0xde);
    uint64_t offset=0;
    CHECK(AstroProducerProbeOffset(256,bytes,source->expected.size(),128,12288,index,&offset));
    std::array<uint32_t,32> original{};
    for(uint32_t i=0;i<32;++i)original[i]=i%3==0?0x7fc12345u:i%3==1?0xffc01234u:0x3f800000u+i;
    std::memcpy(source->map+offset,original.data(),128);
    std::memcpy(source->expected.data()+offset,original.data(),128);
    const AstroProducerBufferCopy copy{source->buffer,offset,256};
    RecordAstroProducerBufferCopies(device.command,before->buffer,before->expected.size(),std::span(&copy,1),ProbeDispatch{});
    std::memcpy(before->expected.data()+256,original.data(),128);
    device.Write({source,offset});
    RecordAstroProducerBufferCopies(device.command,after->buffer,after->expected.size(),std::span(&copy,1),ProbeDispatch{});
    std::memcpy(after->expected.data()+256,source->expected.data()+offset,128);
    device.Submit();
    CHECK(!std::memcmp(before->map+256,original.data(),128));
    for(uint32_t i=0;i<32;++i){uint32_t value;std::memcpy(&value,after->map+256+i*4,4);CHECK(value==0x71100000u+i);}
    std::printf("PASS Vulkan actual production probe copy commands index=%u before_NaN_bits_exact=1 after_compute_words_exact=32 all_guards_exact=1 sync_validation=1 game_frame=0\n",index);
}
static void InputGpu(const char* shader,uint32_t slot,uint32_t stride,uint32_t records,uint32_t index) {
    Device device(true,shader,true);
    const uint64_t bytes=uint64_t(stride)*records;
    auto source=device.Make(bytes,0xa5),destination=device.Make(1024,0xcd);
    uint64_t offset=0;CHECK(AstroProducerInputProbeOffset(256,bytes,source->expected.size(),stride,records,slot,index,&offset));
    for(uint32_t i=0;i<stride/4;++i) {
        const uint32_t value=i%9==0?0x7fc12345u:0x3f000000u+i;
        std::memcpy(source->map+offset+i*4,&value,4);std::memcpy(source->expected.data()+offset+i*4,&value,4);
    }
    const AstroProducerBufferCopy copy{source->buffer,offset,256,stride};
    RecordAstroProducerBufferCopies(device.command,destination->buffer,destination->expected.size(),std::span(&copy,1),ProbeDispatch{});
    std::memcpy(destination->expected.data()+256,source->expected.data()+offset,stride);
    device.Submit();
    std::printf("PASS full producer input GPU copy slot=%u index=%u bytes=%u raw_NaN_and_all_guards_exact=1 sync_validation=1\n",slot,index,stride);
}
static void BounceGpu(const char* shader) {
 Device device(true,shader,true);
 auto source=device.Make(512,0x00),destination=device.Make(256,0xcd);
 // The GPU producer writes 32 words; the separately retained CPU shadow stays zero.
 std::array<uint32_t,31> stale_shadow{};
 device.Write({source,256});
 RecordGpuOwnedBufferBounce(device.command,source->buffer,260,destination->buffer,256,124,ProbeDispatch{});
 std::memcpy(destination->expected.data()+256,source->expected.data()+260,124);
 device.Barrier(VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
 device.Submit();
 CHECK(std::memcmp(destination->map+256,stale_shadow.data(),124)!=0);
 for(uint32_t i=0;i<31;++i) {uint32_t v;std::memcpy(&v,destination->map+256+i*4,4);CHECK(v==0x71100001u+i);}
 std::puts("PASS GPU-owned unaligned SSBO bounce: current compute writes exact, stale zero CPU shadow ignored, all guards exact, synchronization validation0");
}
int main(int argc,char**argv) {
    try{
        CHECK(argc==3);Bounds();
        if(std::string(argv[1])=="--gpu") {
            for(uint32_t index:{0u,12158u,12222u})Gpu(argv[2],index);
            BounceGpu(argv[2]);
            InputGpu(argv[2],4,432,2048,0x7fe);InputGpu(argv[2],3,276,16384,0x3a00);
        } else CHECK(std::string(argv[1])=="--cpu");
        CHECK(validation_issues==0);
        std::printf("PASS producer buffer probe validation_issues=%u\n",validation_issues.load());
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
