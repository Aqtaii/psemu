#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "common/assert.h"
#include <cstdio>
#include <cstdlib>
using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;
namespace Common {
int DbgExitHandler(char const*,int line,std::string_view text) {std::fprintf(stderr,"fatal %d: %.*s\n",line,int(text.size()),text.data());std::abort();}
int DbgExitHandler(char const*,int,fmt::text_style,std::string_view) {std::abort();}
int DbgExitIfHandler(char const*,char const*,int) {std::abort();}
int DbgNotImplementedHandler(char const*,char const*,int) {std::abort();}
void DbgExit(int) {std::abort();}
}
IR::Operand Imm(uint32_t value){IR::Operand o;o.kind=IR::OperandKind::ImmediateU32;o.imm=value;return o;}
IR::Operand Reg(IR::RegisterFile file,uint32_t index){IR::Operand o;o.kind=IR::OperandKind::Register;o.reg={file,index};return o;}
void Require(bool ok,const std::string& error={}){if(!ok){std::fprintf(stderr,"Mesh emitter test failed: %s\n",error.c_str());std::exit(1);}}
int main(){
 IR::Program p; p.stage=ShaderType::Mesh;p.wave_size=32;
 p.srt_patching_complete=p.resource_tracking_complete=p.shader_info_complete=p.binding_layout_complete=true;
 p.info.outputs={{IR::StageOutputKind::Position,0,0,"position"},
                 {IR::StageOutputKind::Parameter,0,0,"color"},
                 {IR::StageOutputKind::Parameter,1,1,"modulation"}};
 p.blocks.resize(1);auto& block=p.blocks[0];block.id=0;
 auto instruction=[&](IR::Opcode op,IR::Operand dst,std::initializer_list<IR::Operand> src){
  IR::Instruction i;i.op=op;i.dst=dst;i.pc=uint32_t(block.instructions.size()*4);
  for(const auto& operand:src)i.src[i.src_count++]=operand;block.instructions.push_back(i);
 };
 const auto vector=[](uint32_t index){return Reg(IR::RegisterFile::Vector,index);};
 const auto mask=Reg(IR::RegisterFile::Vcc,0);
 instruction(IR::Opcode::CompareEqU32,mask,{vector(5),Imm(1)});
 instruction(IR::Opcode::SelectMaskU32,vector(1),{mask,Imm(0x40400000),Imm(0xbf800000)});
 instruction(IR::Opcode::CompareEqU32,mask,{vector(5),Imm(2)});
 instruction(IR::Opcode::SelectMaskU32,vector(2),{mask,Imm(0x40400000),Imm(0xbf800000)});
 instruction(IR::Opcode::MoveU32,Reg(IR::RegisterFile::M0,0),{Imm(3|(1<<12))});
 instruction(IR::Opcode::Sendmsg,{}, {Imm(9),Reg(IR::RegisterFile::M0,0)});
 auto output=[&](IR::ExportTargetKind kind,uint32_t index,std::initializer_list<IR::Operand> src){
  instruction(IR::Opcode::Export,{},src);auto& i=block.instructions.back();
  i.export_info.kind=kind;i.export_info.index=index;i.export_info.en=(1u<<i.src_count)-1;
 };
 output(IR::ExportTargetKind::Position,0,{vector(1),vector(2),Imm(0),Imm(0x3f800000)});
 output(IR::ExportTargetKind::Parameter,0,{Imm(0x3f800000),Imm(0),Imm(0),Imm(0x3f800000)});
 output(IR::ExportTargetKind::Parameter,1,{Imm(0x3f800000),Imm(0x3f800000),Imm(0x3f800000),Imm(0x3f800000)});
 output(IR::ExportTargetKind::Primitive,0,{Imm((1u<<10)|(2u<<20))});
 ShaderVertexInputInfo info{};info.mesh={3,1,32,32,64};
 IR::ResourceSnapshot resources;std::vector<uint32_t> words;std::string error;
 Require(Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error),error);
 auto* file=std::fopen("native_mesh_emitter.spv","wb");Require(file!=nullptr);
 Require(std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size());Require(std::fclose(file)==0);
 info.mesh.invocations=31;Require(!Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error));
 info.mesh.invocations=32;
 auto duplicate=p.info.outputs.back();p.info.outputs.push_back(duplicate);
 Require(!Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error));
 Require(error.find("duplicate")!=std::string::npos,error);p.info.outputs.pop_back();
 p.wave_size=64;Require(!Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error));
 Require(error.find("wave64")!=std::string::npos,error);
 p.wave_size=32;p.dispatcher_fallback=true;
 instruction(IR::Opcode::Barrier,{},{});
 Require(!Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error));
 Require(error.find("uniformity")!=std::string::npos,error);
 std::puts("Native production Mesh emitter passed; wave64 and dispatcher barriers remain gated");
}
