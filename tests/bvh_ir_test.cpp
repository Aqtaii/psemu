#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/ImageOps.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
namespace IR = Libs::Graphics::ShaderRecompiler::IR;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;
namespace Libs::Graphics::ShaderRecompiler::IR {
bool LowerBvhIntersect(const Decoder::Instruction&, BasicBlock*, std::string*);
}
std::vector<IR::Register> BvhCollected(const IR::Instruction&);
static void Check(bool ok) { if (!ok) throw std::runtime_error("BVH IR regression"); }
int main() {
 for (uint32_t dim: {0u,1u}) {
  std::array<uint32_t,2> store{0xf0000000u | (8u<<18u) | (dim<<3u) | (15u<<8u),0x00040105u};
  Decoder::Instruction decoded;std::string error;
  Check(Decoder::DecodeMimg(0,store,0,&decoded,&error));
  Check(decoded.opcode==Decoder::Opcode::ImageStore);
  Check(decoded.image_dimension==(dim==0?Decoder::ImageDimension::Dim1D:Decoder::ImageDimension::Dim2D));
  Check(decoded.image_address_components==(dim==0?1u:2u));
 }
 std::array<uint32_t,5> words{0xf1989f07,0x00040505,0x4442413d,0x4543403e,0x00004746};
 constexpr std::array<uint32_t,11> ray{5,61,65,66,68,62,64,67,69,70,71};
 for (bool wide: {false,true}) for (bool nsa: {false,true}) {
  auto raw=words;if(wide)raw[0]+=0x40000;if(!nsa)raw[0]&=~6u;
  Decoder::Instruction decoded;std::string error;
  Check(Decoder::DecodeMimg(0x2190,raw,0,&decoded,&error));
  IR::BasicBlock block;Check(IR::LowerBvhIntersect(decoded,&block,&error));
  Check(block.instructions.size()==1);
  const auto& inst=block.instructions.front();
  Check(inst.op==IR::Opcode::BvhIntersect && inst.pc==0x2190);
  Check(inst.dst.reg==IR::Register{IR::RegisterFile::Vector,5});
  Check(inst.src_count==4 && inst.memory.kind==IR::ResourceKind::None);
  Check(inst.memory.data_dwords==4 && inst.bvh.node_dwords==(wide?2u:1u));
  Check(inst.bvh.ray.size()==(wide?12u:11u));
  for(uint32_t i=0;i<4;++i) Check(inst.src[i].reg==IR::Register{IR::RegisterFile::Scalar,16+i});
  for(uint32_t i=0;i<inst.bvh.ray.size();++i) {
   const auto reg=nsa?(i<ray.size()?ray[i]:0u):5+i;
   Check(inst.bvh.ray[i].reg==IR::Register{IR::RegisterFile::Vector,reg});
  }
  const auto collected=BvhCollected(inst);
  auto has=[&](IR::RegisterFile file,uint32_t index){return std::find(collected.begin(),collected.end(),IR::Register{file,index})!=collected.end();};
  for(uint32_t i=0;i<4;++i){Check(has(IR::RegisterFile::Scalar,16+i));Check(has(IR::RegisterFile::Vector,5+i));}
  for(const auto& operand:inst.bvh.ray)Check(has(operand.reg.file,operand.reg.index));
  Check(has(IR::RegisterFile::Exec,0) && has(IR::RegisterFile::Exec,1));
  if(nsa)Check(!has(IR::RegisterFile::Vector,9));
  // Captured v5 is both an input and the first output: lowering must remain one op.
  Check(inst.bvh.ray.front().reg==inst.dst.reg);
  auto copy=inst;Check(copy==inst);copy.bvh.ray[1].reg.index^=1;Check(!(copy==inst));
  for(uint32_t malformed=0;malformed<7;++malformed) {
   auto bad=decoded;
   if(malformed==0)bad.image_address_components=10;
   if(malformed==1)bad.image_r128=false;
   if(malformed==2)bad.dmask=7;
   if(malformed==3)bad.image_nsa_dwords=1;
   if(malformed==4)bad.dst.reg=253;
   if(malformed==5)bad.src1.kind=Decoder::OperandKind::Vgpr;
   if(malformed==6){bad.image_nsa_dwords=0;bad.src0.reg=250;}
   IR::BasicBlock rejected;Check(!IR::LowerBvhIntersect(bad,&rejected,&error));
   Check(rejected.instructions.empty() && !error.empty());
  }
 }
 std::puts("PASS: captured BVH decoder-to-IR, live descriptor, NSA/wide rays and overlap");
}
