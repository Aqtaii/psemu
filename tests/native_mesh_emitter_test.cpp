#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"
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
// Exercise the production register allocator/load/store emitter without enabling public wave64.
// A render readback must distinguish upper/lower VGPR values while seeing a shared scalar write.
void WriteLaneBackingFixture(const IR::Program& program, const ShaderVertexInputInfo& info,
                             const IR::ResourceSnapshot& resources) {
 using namespace Spirv::Emitter;
 EmitterState state;state.builder=Spirv::Builder(0x00010400u);
 state.program=&program;state.resources=&resources;state.vertex_input_info=&info;
 state.stage=ShaderType::Mesh;state.wave_size=32;state.lanes_per_invocation=2;
 CollectRegisters(program,&state.registers);
 const IR::Register value{IR::RegisterFile::Vector,9};
 const IR::Register shared{IR::RegisterFile::Scalar,24};
 CollectRegister(&state.registers,value);CollectRegister(&state.registers,shared);
 CopyProgramInputsAndOutputs(&state,program);
 state.needs_subgroup_ballot=ProgramNeedsSubgroupBallot(program);
 state.needs_subgroup_shuffle=ProgramNeedsSubgroupShuffle(program);
 state.needs_subgroup_local_invocation_id=ProgramNeedsSubgroupLocalInvocationId(program);
 AllocateInputVariables(&state);AllocateOutputVariables(&state);
 AllocateDescriptorVariables(&state);EmitHeaderAndTypes(&state);AllocateRegisterVariables(&state);
 Require(PointerForRegisterHalf(state,value,0)!=0);
 Require(PointerForRegisterHalf(state,value,0)!=PointerForRegisterHalf(state,value,1));
 Require(PointerForRegisterHalf(state,shared,0)==PointerForRegisterHalf(state,shared,1));
 Require(PointerForRegisterHalf(state,value,2)==0);
 Require(PointerForRegisterHalf(state,{IR::RegisterFile::Vector,999},0)==0);
 state.builder.AddFunction({OpFunction,state.void_type,state.mesh_guest_func,FunctionControlNone,state.func_type});
 state.builder.AddFunction({OpLabel,state.builder.AllocateId()});
 EmitRegisterVariables(&state);
 const auto index=state.builder.AllocateId();
 state.builder.AddFunction({OpLoad,state.uint_type,index,state.mesh_local_index});
 state.builder.AddFunction({OpStore,PointerForRegister(state,{IR::RegisterFile::Vector,5}),index});
 EmitStoreU32(&state,Reg(value.file,value.index),ConstantU32(&state,0x3e800000)); // 0.25
 EmitStoreU32(&state,Reg(shared.file,shared.index),ConstantU32(&state,0));
 state.lane_half=1;
 EmitStoreU32(&state,Reg(value.file,value.index),ConstantU32(&state,0x3f400000)); // 0.75
 EmitStoreU32(&state,Reg(shared.file,shared.index),ConstantU32(&state,0x3f800000)); // 1
 const auto upper=EmitBitcastU32ToF32(&state,EmitRegisterLoad(&state,value));
 state.lane_half=0;
 const auto lower=EmitBitcastU32ToF32(&state,EmitRegisterLoad(&state,value));
 const auto sum=state.builder.AllocateId();
 state.builder.AddFunction({OpFAdd,state.float_type,sum,lower,upper});
 EmitStoreU32(&state,Reg(value.file,value.index),EmitBitcastF32ToU32(&state,sum));
 for(const auto& original:program.blocks[0].instructions) {
  auto inst=original;
  if(inst.op==IR::Opcode::Export && inst.export_info.kind==IR::ExportTargetKind::Parameter) {
   if(inst.export_info.index==0) inst.src[0]=Reg(value.file,value.index);
   if(inst.export_info.index==1) inst.src[0]=Reg(shared.file,shared.index);
  }
  EmitInstruction(&state,inst);
 }
 state.builder.AddFunction({OpReturn});state.builder.AddFunction({OpFunctionEnd});
 EmitNativeMeshEntryPoint(&state);
 const auto words=state.builder.Build();
 auto* file=std::fopen("native_mesh_lane_backing.spv","wb");Require(file!=nullptr);
 Require(std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size());
 Require(std::fclose(file)==0);
}
int main(int argc,char** argv){
 Require(argc==1 || argc==2);
 const uint32_t invocations=argc==2 ? 64u : 32u;
 if(argc==2) Require(std::string(argv[1])=="multiwave");
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
 ShaderVertexInputInfo info{};info.mesh={3,1,invocations,32,64};
 IR::ResourceSnapshot resources;std::vector<uint32_t> words;std::string error;
 Require(Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error),error);
 auto* file=std::fopen(argc==2 ? "native_mesh_emitter_multiwave.spv" : "native_mesh_emitter.spv","wb");Require(file!=nullptr);
 Require(std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size());Require(std::fclose(file)==0);
 if(argc==1) WriteLaneBackingFixture(p,info,resources);
 info.mesh.invocations=31;Require(!Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error));
 info.mesh.invocations=invocations;
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
