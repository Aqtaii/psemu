#include "graphics/shader/recompiler/ScalarAluOps.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "common/assert.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;
namespace Common {
int DbgExitHandler(char const*,int line,std::string_view text){std::fprintf(stderr,"fatal %d: %.*s\n",line,int(text.size()),text.data());std::abort();}
int DbgExitHandler(char const*,int,fmt::text_style,std::string_view){std::abort();}
int DbgExitIfHandler(char const*,char const*,int){std::abort();}
int DbgNotImplementedHandler(char const*,char const*,int){std::abort();}
void DbgExit(int){std::abort();}
}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"SOPK test line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IR::Operand Imm(uint32_t x){IR::Operand o;o.kind=IR::OperandKind::ImmediateU32;o.imm=x;return o;}
IR::Operand Reg(IR::RegisterFile f,uint32_t i){IR::Operand o;o.kind=IR::OperandKind::Register;o.reg={f,i};return o;}
IR::Program Lower(std::span<const uint32_t> code,ShaderType stage=ShaderType::Pixel) {
 Decoder::Program decoded;std::string error;CHECK(Decoder::DecodeProgram(code,&decoded,&error));
 CFG::Graph graph;CHECK(CFG::BuildGraph(decoded,&graph,&error));
 CHECK(CFG::StructurizeForLowering(&graph,&error));
 IR::Program p;CHECK(IR::LowerProgram(decoded,graph,stage,32,&p,&error));return p;
}
void WriteFragment(){
 // Real Astro GS instruction at PC0x33e4. One real triangle => packed count > 0xffff.
 const std::array<uint32_t,4> code{0xbe8403ffu,0x00010003u,0xb704ffffu,0xbf810000u};
 auto p=Lower(code);CHECK(!p.blocks.empty());
 p.lane_mask_mode=ShaderLaneMaskMode::NativeWave;
 p.srt_patching_complete=p.resource_tracking_complete=p.shader_info_complete=p.binding_layout_complete=true;
 p.info.outputs={{IR::StageOutputKind::Mrt,0,0,"color"}};

 IR::Instruction select;select.op=IR::Opcode::SelectU32;select.execution=IR::InstructionExecution::Scalar;
 select.dst=Reg(IR::RegisterFile::Scalar,5);select.src_count=3;
 select.src[0]=Reg(IR::RegisterFile::Scc,static_cast<uint32_t>(Decoder::OperandKind::Scc));select.src[1]=Imm(0);select.src[2]=Imm(0x3f800000);

 IR::Instruction output;output.op=IR::Opcode::Export;output.execution=IR::InstructionExecution::Vector;
 output.src_count=4;output.src[0]=Reg(IR::RegisterFile::Scalar,5);
 output.src[1]=output.src[2]=Imm(0);output.src[3]=Imm(0x3f800000);
 output.export_info.kind=IR::ExportTargetKind::Mrt;output.export_info.en=15;
 unsigned returns=0;for(auto& block:p.blocks)if(block.terminator.kind==CFG::TerminatorKind::Return){
  block.instructions.push_back(select);block.instructions.push_back(output);++returns;}CHECK(returns>0);
 ShaderPixelInputInfo pixel{};pixel.target_output_mode[0]=4;
 IR::ResourceSnapshot resources;std::vector<uint32_t> words;std::string error;
 if(!Spirv::EmitProgram(p,resources,nullptr,&pixel,nullptr,&words,&error))
  throw std::runtime_error(error);
#ifdef EXPECT_OLD_SOPK
 const char* filename="sopk_old_fragment.spv";
#else
 const char* filename="sopk_fixed_fragment.spv";
#endif
 std::ofstream file(filename,std::ios::binary);file.write(reinterpret_cast<const char*>(words.data()),words.size()*4);CHECK(file);
}
int main(){try{
 std::string error;
 const std::array<uint32_t,1> captured{0xb704ffffu};Decoder::Instruction inst;
 CHECK(Decoder::DecodeSopk(0x33e4,captured,0,&inst,&error));
 CHECK(inst.opcode==Decoder::Opcode::SCmpLeU32 && inst.pc==0x33e4 && inst.src0.reg==4);
#ifdef EXPECT_OLD_SOPK
 CHECK(inst.src1.value==0xffffffffu);
 CHECK(uint32_t(0x10003)<=inst.src1.value); // Wrongly selects empty fallback.
 WriteFragment();puts("Old production decoder control: Astro unsigned immediate becomes 0xffffffff (reproduced)");
#else
 CHECK(inst.src1.value==0xffffu && inst.src1.signed_val==65535);
 CHECK(!(uint32_t(0x10003)<=inst.src1.value));
 unsigned cases=0;
 for(uint32_t op=3;op<=14;++op)for(uint32_t bits:{0u,1u,0x7fffu,0x8000u,0xfffeu,0xffffu}){
  const std::array<uint32_t,2> code{0xb0000000u|(op<<23)|(4u<<16)|bits,0xbf810000u};
  Decoder::Program decoded;CHECK(Decoder::DecodeProgram(code,&decoded,&error));
  const auto expected=op>=9?bits:uint32_t(int32_t(int16_t(bits)));
  const auto& i=decoded.instructions[0];CHECK(i.family==Decoder::Family::SOPK);
  CHECK(i.src1.value==expected && i.src1.signed_val==int32_t(expected));
  const auto p=Lower(code);CHECK(p.blocks[0].instructions[0].src[1].imm==expected);
  ++cases;
 }
 // Signed moves/adds/multiplies retain sign extension.
 for(uint32_t op:{0u,15u,16u}) {
  const std::array<uint32_t,1> code{0xb0000000u|(op<<23)|(4u<<16)|0xffffu};
  Decoder::Instruction i;CHECK(Decoder::DecodeSopk(0,code,0,&i,&error));
  CHECK((op==0?i.src0:i.src1).value==0xffffffffu);
 }
 WriteFragment();printf("SOPK production decoder/IR: %u signed/unsigned boundary cases, captured GS predicate and signed ALU controls PASS\n",cases);
#endif
}catch(const std::exception& e){std::fprintf(stderr,"SOPK emission: %s\n",e.what());return 1;}}