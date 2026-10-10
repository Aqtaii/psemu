#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define KYTY_SYSV_ABI __attribute__((sysv_abi))
namespace Pm4 {constexpr uint32_t IT_DMA_DATA=0x50;}
#define KYTY_PM4(n,op,flags) (0xc0000000u|((n-2u)<<16u)|((op)<<8u)|(flags))
struct CommandBuffer {std::array<uint32_t,7> words{};uint32_t* AllocateDW(uint32_t n){return n==7?words.data():nullptr;}};

// PRODUCTION DMA BUILDERS
static void Check(bool ok){if(!ok)throw std::runtime_error("ACB DMA ABI mismatch");}
int main() {
 CommandBuffer buffer;
 using GuestCall=uint32_t*(KYTY_SYSV_ABI*)(CommandBuffer*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
 auto call=reinterpret_cast<GuestCall>(&GraphicsAcbDmaData);
 constexpr uint64_t destination=0x1234567800020000ull,source=0x2345678900040000ull;
 auto* words=call(&buffer,0,2,destination,2,1,source,4,1,1);
 Check(words==buffer.words.data());
 Check(words[0]==0xc0055000u);
 Check(words[1]==((1u<<13u)|(2u<<25u)|(2u<<29u)));
 Check(words[2]==uint32_t(source)&&words[3]==uint32_t(source>>32u));
 Check(words[4]==uint32_t(destination)&&words[5]==uint32_t(destination>>32u));
 Check(words[6]==(4u|(1u<<30u)|(1u<<31u)));
 words=call(&buffer,3,0,destination,3,0,source,4097,0,0);
 Check(words[1]==((3u<<20u)|(3u<<29u)));
 Check(words[2]==uint32_t(source)&&words[4]==uint32_t(destination)&&words[6]==4097);
 Check(GraphicsAcbDmaData(nullptr,0,0,destination,2,0,source,4,0,0)==nullptr);
 std::puts("PASS: actual ten-argument SysV ACB DMA call preserves packet addresses and byte count");
}
