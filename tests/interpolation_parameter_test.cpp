#include "graphics/shader/recompiler/VectorAluOps.h"
#include "graphics/shader/recompiler/ShaderInfoCollection.h"
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
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"Interpolation test line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IR::Operand Imm(uint32_t x){IR::Operand o;o.kind=IR::OperandKind::ImmediateU32;o.imm=x;return o;}
IR::Operand Reg(IR::RegisterFile f,uint32_t i){IR::Operand o;o.kind=IR::OperandKind::Register;o.reg={f,i};return o;}
IR::Program Lower(std::span<const uint32_t> code,ShaderType stage=ShaderType::Pixel,uint32_t wave=32) {
 Decoder::Program decoded;std::string error;CHECK(Decoder::DecodeProgram(code,&decoded,&error));
 CFG::Graph graph;CHECK(CFG::BuildGraph(decoded,&graph,&error));
 CHECK(CFG::StructurizeForLowering(&graph,&error));
 IR::Program p;CHECK(IR::LowerProgram(decoded,graph,stage,wave,&p,&error));return p;
}

void WriteCase(unsigned which) {
 const bool flat=which==4,custom=which==1,mixed=which==2 || which==3;
 const std::array<uint32_t,4> code{0xc8020000,0xc8060001,0xc80a0002,0xbf810000};
 auto p=Lower(flat?std::span<const uint32_t>(code).subspan(2):std::span<const uint32_t>(code));
 ShaderPixelInputInfo pixel{};pixel.input_num=1;pixel.target_output_mode[0]=4;
 pixel.custom_interpolation_mask=custom?1:0;pixel.ps_no_perspective=which==3;
 if(flat)pixel.interpolator_settings[0]=0x400;
 if(mixed)for(auto& b:p.blocks)if(b.terminator.kind==CFG::TerminatorKind::Return){
  IR::Instruction load;load.op=IR::Opcode::LoadInputF32;load.dst=Reg(IR::RegisterFile::Vector,3);
  load.execution=IR::InstructionExecution::Vector;b.instructions.push_back(load);
 }
 p.resource_tracking_complete=true; IR::ShaderInfoOptions options;options.pixel=&pixel;std::string error;
 CHECK(IR::CollectShaderInfo(&p,options,&error));
 CHECK(p.info.inputs[0].per_vertex==!flat);
 p.lane_mask_mode=ShaderLaneMaskMode::NativeWave;
 p.srt_patching_complete=p.resource_tracking_complete=p.shader_info_complete=p.binding_layout_complete=true;
 p.info.outputs={{IR::StageOutputKind::Mrt,0,0,"color"}};
 for(auto& b:p.blocks)if(b.terminator.kind==CFG::TerminatorKind::Return){
  IR::Instruction o;o.op=IR::Opcode::Export;o.execution=IR::InstructionExecution::Vector;
  o.src_count=4;o.src[0]=Reg(IR::RegisterFile::Vector,flat?2:0);
  o.src[1]=flat?Imm(0):Reg(IR::RegisterFile::Vector,1);
  o.src[2]=flat?Imm(0):Reg(IR::RegisterFile::Vector,mixed?3:2);o.src[3]=Imm(0x3f800000);
  o.export_info.kind=IR::ExportTargetKind::Mrt;o.export_info.en=15;b.instructions.push_back(o);
 }
 IR::ResourceSnapshot resources;std::vector<uint32_t> words;
 if(!Spirv::EmitProgram(p,resources,nullptr,&pixel,nullptr,&words,&error))throw std::runtime_error(error);
 bool bary_cap=false;
 for(size_t i=5;i<words.size();){const auto count=words[i]>>16;CHECK(count && i+count<=words.size());
  bary_cap|=(words[i]&65535)==17 && count==2 && words[i+1]==5284;i+=count;}
 CHECK(bary_cap==!flat);
 std::ofstream f("interpolation_"+std::to_string(which)+".spv",std::ios::binary);
 f.write(reinterpret_cast<const char*>(words.data()),words.size()*4);CHECK(f);
}
int main(int argc,char**argv){try{
 std::string error;
 const std::array<uint32_t,1> actual{0xc83e0800u};Decoder::Instruction captured;
 CHECK(Decoder::DecodeVintrp(0x150,actual,0,&captured,&error));
 CHECK(captured.src0.value==0 && captured.src1.value==2 && captured.src2.value==0 && captured.dst.reg==15);
 for(uint32_t mode=0;mode<4;mode++){
  const std::array<uint32_t,2> code{0xc8020000|mode,0xbf810000};
  Decoder::Instruction decoded;CHECK(Decoder::DecodeVintrp(0x150,code,0,&decoded,&error));
  CHECK(decoded.opcode==Decoder::Opcode::VInterpMovF32 && decoded.src0.value==mode);
  Decoder::Program d;CHECK(Decoder::DecodeProgram(code,&d,&error));CFG::Graph g;
  CHECK(CFG::BuildGraph(d,&g,&error));CHECK(CFG::StructurizeForLowering(&g,&error));
  IR::Program p;CHECK(IR::LowerProgram(d,g,ShaderType::Pixel,32,&p,&error)==(mode<3));
 }
 for(unsigned i=0;i<5;i++)WriteCase(i);
 if(argc==2){
  std::ifstream f(argv[1],std::ios::binary|std::ios::ate);CHECK(f);const auto bytes=f.tellg();
  CHECK(bytes>0 && bytes%4==0);std::vector<uint32_t> words(size_t(bytes)/4);f.seekg(0);
  f.read(reinterpret_cast<char*>(words.data()),bytes);CHECK(f);
  auto p=Lower(words,ShaderType::Pixel,64);size_t moves=0;
  for(const auto&b:p.blocks)for(const auto&i:b.instructions)moves+=i.op==IR::Opcode::LoadInterpolationParameterF32;
  CHECK(moves>0);printf("Captured PS all-block lowering PASS wave=64 blocks=%zu interpolation_parameters=%zu\n",p.blocks.size(),moves);
 }
 puts("PASS VINTRP modes0/1/2, reserved3 rejection, per-vertex/flat/custom and mixed interpolation emitter cases");
 return 0;
}catch(const std::exception&e){fprintf(stderr,"Interpolation test: %s\n",e.what());return 1;}}
