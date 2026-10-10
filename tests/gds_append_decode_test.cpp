#include "graphics/shader/recompiler/MemoryOps.h"
#include "graphics/shader/recompiler/VectorAluOps.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace Libs::Graphics::ShaderRecompiler::Decoder;
static void Check(bool ok) {if(!ok) throw std::runtime_error("GDS decoder regression");}
int main() {
 // Actual Astro Bot instruction; preserve scalar register-pair sources and VCC destination.
 const std::array<uint32_t,2> astro{0xd4e4006a,0x0001006a};
 Instruction compare;std::string compare_error;
 Check(DecodeVop3(0x784,astro,0,&compare,&compare_error));
 Check(compare.opcode==Opcode::VCmpGtU64 && compare.src_count==2);
 Check(compare.dst.kind==OperandKind::VccLo && compare.src0.kind==OperandKind::VccLo);

 for(uint32_t opcode: {0x3du,0x3eu}) {
  for(uint32_t offset: {0u,4u,8u,0xffu,0x100u,0xc68u,0xffffu}) {
   const std::array<uint32_t,2> code{0xd8000000u|(opcode<<18u)|(1u<<17u)|offset,0};
   Instruction inst; std::string error;
   Check(DecodeDs(0xbf4,code,0,&inst,&error));
#if EXPECT_OLD_GDS_REJECTION
   Check(inst.opcode==(offset?Opcode::Unsupported:(opcode==0x3e?Opcode::DsAppend:Opcode::DsConsume)));
#else
   Check(inst.opcode==(opcode==0x3e?Opcode::DsAppend:Opcode::DsConsume));
#endif
   Check(inst.gds && inst.offset==offset && inst.word_count==2 && inst.pc==0xbf4);
  }
 }
 // Keep unrelated invalid GDS swizzle restrictions in place.
 const std::array<uint32_t,2> invalid{0xd8000000u|(0x35u<<18u)|(1u<<17u),0};
 Instruction inst; std::string error;
 Check(DecodeDs(0,invalid,0,&inst,&error) && inst.opcode==Opcode::Unsupported);
 std::puts("PASS: real DS append/consume decoding preserves GDS byte offsets");
}
