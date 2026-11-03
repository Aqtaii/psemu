#include "compute_probe_device.inc"
#include "graphics/host_gpu/renderer/computeBufferProbe.h"
#include <limits>
using namespace Libs::Graphics;
struct ProbeDispatch {
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier=::vkCmdPipelineBarrier;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer=::vkCmdCopyBuffer;
};

static void Bounds() {
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
int main(int argc,char**argv) {
    try{
        CHECK(argc==3);Bounds();
        if(std::string(argv[1])=="--gpu")
            for(uint32_t index:{0u,12158u,12222u})Gpu(argv[2],index);
        else CHECK(std::string(argv[1])=="--cpu");
        CHECK(validation_issues==0);
        std::printf("PASS producer buffer probe validation_issues=%u\n",validation_issues.load());
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
