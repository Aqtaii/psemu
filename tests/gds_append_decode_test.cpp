#include "graphics/shader/recompiler/MemoryOps.h"
#include "graphics/shader/recompiler/ScalarAluOps.h"
#include "graphics/shader/recompiler/ImageOps.h"
#include "graphics/shader/recompiler/VectorAluOps.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace Libs::Graphics::ShaderRecompiler::Decoder;
static void Check(bool ok) {if(!ok) throw std::runtime_error("GDS decoder regression");}
int main() {
 // Captured Astro instruction: v5 output, s[16:19] BVH descriptor, 11 ray words.
 std::array<uint32_t,5> bvh{0xf1989f07,0x00040505,0x4442413d,0x4543403e,0x00004746};
 Instruction ray;std::string ray_error;
 Check(DecodeMimg(0x2190,bvh,0,&ray,&ray_error));
 Check(ray.opcode==Opcode::ImageBvhIntersectRay && ray.opcode_id==0xe6);
 Check(ray.word_count==5 && ray.image_nsa_dwords==3 && ray.image_address_components==11);
 Check(ray.image_r128 && ray.dmask==15 && ray.data_dwords==4);
 Check(ray.dst.kind==OperandKind::Vgpr && ray.dst.reg==5);
 Check(ray.src0.kind==OperandKind::Vgpr && ray.src0.reg==5);
 Check(ray.src1.kind==OperandKind::Sgpr && ray.src1.reg==16);
 constexpr std::array<uint32_t,10> addresses{61,65,66,68,62,64,67,69,70,71};
 for(unsigned i=0;i<addresses.size();i++) Check(ray.image_nsa_addr[i]==addresses[i]);
 auto wide=bvh;wide[0]+=0x40000;
 Check(DecodeMimg(0,wide,0,&ray,&ray_error) && ray.opcode==Opcode::ImageBvhIntersectRay);
 Check(ray.image_address_components==12);
 auto sequential=bvh;sequential[0]&=~6u;
 Check(DecodeMimg(0,std::span<const uint32_t>(sequential.data(),2),0,&ray,&ray_error));
 Check(ray.opcode==Opcode::ImageBvhIntersectRay && ray.word_count==2);
 for(unsigned malformed=0;malformed<5;malformed++) {
  auto bad=bvh;
  if(malformed==0) bad[0]&=~0x8000u; // R128 off
  if(malformed==1) bad[0]&=~0x100u;  // partial output mask
  if(malformed==2) bad[1]|=0x40000000u; // A16
  if(malformed==3) bad[1]|=0x80000000u; // D16
  if(malformed==4) bad[0]=(bad[0]&~6u)|2u; // incomplete NSA
  Check(DecodeMimg(0,bad,0,&ray,&ray_error) && ray.opcode==Opcode::Unsupported);
 }
 Check(!DecodeMimg(0,std::span<const uint32_t>(bvh.data(),4),0,&ray,&ray_error));


 const std::array<uint32_t,1> ff1_raw{0xbeeb146a};
 Instruction ff1;std::string ff1_error;
 Check(DecodeSop1(0x2280,ff1_raw,0,&ff1,&ff1_error));
 Check(ff1.opcode==Opcode::SFf1I32B64 && ff1.src_count==1);
 Check(ff1.src0.kind==OperandKind::VccLo && ff1.dst.kind==OperandKind::VccHi);
 // Astro's exact S_TRAP word: preserve immediate and fall-through classification.
 const std::array<uint32_t,1> trap_code{0xbf920001};
 Instruction trap;std::string trap_error;
 Check(DecodeSopp(0x1f80,trap_code,0,&trap,&trap_error));
 Check(trap.opcode==Opcode::STrap && trap.src_count==1 && trap.src0.value==1);
 Check(trap.pc==0x1f80 && trap.word_count==1 && trap.opcode_id==0x12);
 Check(trap.opcode!=Opcode::SBranch && trap.opcode!=Opcode::SEndpgm);

 const std::array<uint32_t,1> subrev{0x54003080};
 Instruction subtract;std::string subtract_error;
 Check(DecodeVop2(0xb84,subrev,0,&subtract,&subtract_error));
 Check(subtract.opcode==Opcode::VSubrevCoCiU32 && subtract.src_count==3);
 Check(subtract.src1.kind==OperandKind::Vgpr && subtract.src1.reg==24);
 Check(subtract.src2.kind==OperandKind::VccLo && subtract.dst2.kind==OperandKind::VccLo);
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
