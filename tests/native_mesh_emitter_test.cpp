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
                             const IR::ResourceSnapshot& resources, bool upper_outputs=false) {
 using namespace Spirv::Emitter;
 EmitterState state;state.builder=Spirv::Builder(0x00010400u);
 auto mesh_info=info;
 if(upper_outputs){mesh_info.mesh.max_vertices=35;mesh_info.mesh.max_primitives=33;}
 state.program=&program;state.resources=&resources;state.vertex_input_info=&mesh_info;
 state.stage=ShaderType::Mesh;state.wave_size=64;state.lanes_per_invocation=2;
 state.exact_subgroup_operations=Spirv::ProgramRequiresExactSubgroupSize(program);
 CollectRegisters(program,&state.registers);
 const IR::Register value{IR::RegisterFile::Vector,9};
 const IR::Register shared{IR::RegisterFile::Scalar,24};
 CollectRegister(&state.registers,value);CollectRegister(&state.registers,shared);
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,25});
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,26});
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,27});
 CollectRegister(&state.registers,{IR::RegisterFile::Vector,10});
 CollectRegister(&state.registers,{IR::RegisterFile::Vector,11});
 CollectRegister(&state.registers,{IR::RegisterFile::Vector,12});
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,28});
 CopyProgramInputsAndOutputs(&state,program);
 state.needs_subgroup_ballot=ProgramNeedsSubgroupBallot(program);
 state.needs_subgroup_shuffle=true;
 state.needs_subgroup_local_invocation_id=true;
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
 const auto last_lane=state.builder.AllocateId();
 state.builder.AddFunction({OpIEqual,state.bool_type,last_lane,EmitSubgroupLocalInvocationId(&state),ConstantU32(&state,31)});
 EmitStoreU32(&state,Reg(value.file,value.index),EmitSelectU32Value(&state,last_lane,ConstantU32(&state,0x3e800000),ConstantU32(&state,0))); // lane31: 0.25
 EmitStoreU32(&state,Reg(shared.file,shared.index),ConstantU32(&state,0));
 state.lane_half=1;
 EmitStoreU32(&state,Reg(value.file,value.index),EmitSelectU32Value(&state,last_lane,ConstantU32(&state,0x3f400000),ConstantU32(&state,0))); // lane63: 0.75
 EmitStoreU32(&state,Reg(shared.file,shared.index),ConstantU32(&state,0x3f800000)); // 1
 state.lane_half=0;
 IR::Instruction read;read.op=IR::Opcode::ReadLaneU32;read.src_count=2;
 read.src[0]=Reg(value.file,value.index);read.src[1]=Imm(63);
 read.dst=Reg(IR::RegisterFile::Scalar,25);EmitReadLaneU32(&state,read);
 read.src[1]=Imm(31);read.dst=Reg(IR::RegisterFile::Scalar,26);EmitReadLaneU32(&state,read);
 const auto upper=EmitBitcastU32ToF32(&state,EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,25}));
 const auto lower=EmitBitcastU32ToF32(&state,EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,26}));
 const auto sum=state.builder.AllocateId();
 state.builder.AddFunction({OpFAdd,state.float_type,sum,lower,upper});
 EmitStoreU32(&state,Reg(value.file,value.index),EmitBitcastF32ToU32(&state,sum));
 // Different ballots must survive both half emissions without overwriting each other.
 const auto first_lane=state.builder.AllocateId();
 state.builder.AddFunction({OpIEqual,state.bool_type,first_lane,EmitSubgroupLocalInvocationId(&state),ConstantU32(&state,0)});
 const auto vcc=Reg(IR::RegisterFile::Vcc,0);
 EmitLaneMaskPairFromBool(&state,vcc,last_lane);
 state.lane_half=1;EmitLaneMaskPairFromBool(&state,vcc,first_lane);state.lane_half=0;
 uint32_t passed=0;
 auto check=[&](uint32_t actual,uint32_t expected){
  const auto equal=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,equal,actual,ConstantU32(&state,expected)});
  if(passed==0){passed=equal;}else{
   const auto both=state.builder.AllocateId();
   state.builder.AddFunction({OpLogicalAnd,state.bool_type,both,passed,equal});passed=both;
  }
 };
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,24}),0x3f800000);
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,0}),0x80000000);
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,1}),1);
 auto read_check=[&](uint32_t vgpr,uint32_t lane,uint32_t expected){
  read.src[0]=Reg(IR::RegisterFile::Vector,vgpr);read.src[1]=Imm(lane);
  read.dst=Reg(IR::RegisterFile::Scalar,27);EmitReadLaneU32(&state,read);
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,27}),expected);
 };
 // Sparse EXEC must guard VGPR writes according to the selected guest mask word.
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,2));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,4));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,10),ConstantU32(&state,7));
 state.lane_half=1;EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,10),ConstantU32(&state,9));
 state.lane_half=0;
 read_check(10,1,7);read_check(10,34,9);read_check(10,0,0);read_check(10,32,0);
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,0xffffffff));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,0xffffffff));
 // MBCNT low/high with full masks must recover guest IDs across the 31/32 boundary.
 IR::Instruction count;count.dst=Reg(IR::RegisterFile::Vector,11);count.src_count=2;
 count.src[0]=Imm(0xffffffff);
 for(uint32_t half=0;half<2;++half){
  state.lane_half=half;count.src[1]=Imm(0);EmitMaskedBitCountU32(&state,count,0);
  count.src[1]=count.dst;EmitMaskedBitCountU32(&state,count,1);
 }
 state.lane_half=0;read_check(11,31,31);read_check(11,32,32);read_check(11,63,63);
 // Paired vector ALU must update both banks, while scalar ALU runs once.
 EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,12),ConstantU32(&state,4));
 state.lane_half=1;EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,12),ConstantU32(&state,7));
 state.lane_half=0;
 IR::Instruction paired;paired.op=IR::Opcode::IAddU32;paired.src_count=2;
 paired.execution=IR::InstructionExecution::Vector;
 paired.dst=Reg(IR::RegisterFile::Vector,12);paired.src[0]=paired.dst;paired.src[1]=Imm(3);
 Require(EmitGuestInstruction(&state,paired));
 Require(state.instruction_register_values.empty() && state.lane_half==0);
 read_check(12,0,7);read_check(12,32,10);
 paired.execution=IR::InstructionExecution::Scalar;
 paired.dst=Reg(IR::RegisterFile::Scalar,28);paired.src[0]=paired.dst;paired.src[1]=Imm(1);
 Require(EmitGuestInstruction(&state,paired));
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,28}),1);
 // Both halves compare the pre-instruction VCC low word. Lower writes must not change upper input.
 EmitStoreU32(&state,Reg(IR::RegisterFile::Vcc,0),ConstantU32(&state,0));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Vcc,1),ConstantU32(&state,0));
 paired.op=IR::Opcode::CompareEqU32;paired.execution=IR::InstructionExecution::Vector;
 paired.dst=Reg(IR::RegisterFile::Vcc,0);paired.src[0]=paired.dst;paired.src[1]=Imm(0);
 Require(EmitGuestInstruction(&state,paired));
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,0}),0xffffffff);
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,1}),0xffffffff);
 paired.execution=IR::InstructionExecution::Unknown;Require(!EmitGuestInstruction(&state,paired));
 paired.execution=IR::InstructionExecution::Scalar;paired.op=IR::Opcode::Barrier;
 Require(!EmitGuestInstruction(&state,paired));
 paired.execution=IR::InstructionExecution::Vector;paired.op=IR::Opcode::DsWriteB32;
 Require(!EmitGuestInstruction(&state,paired));
 Require(state.instruction_register_values.empty() && state.lane_half==0);
 EmitStoreU32(&state,Reg(shared.file,shared.index),EmitSelectU32Value(&state,passed,ConstantU32(&state,0x3f800000),ConstantU32(&state,0)));
 for(const auto& original:program.blocks[0].instructions) {
  auto inst=original;
  if(inst.op==IR::Opcode::Export && inst.export_info.kind==IR::ExportTargetKind::Parameter) {
   if(inst.export_info.index==0) inst.src[0]=Reg(value.file,value.index);
   if(inst.export_info.index==1) inst.src[0]=Reg(shared.file,shared.index);
  }
  if(upper_outputs && inst.op==IR::Opcode::MoveU32 && inst.dst.kind==IR::OperandKind::Register && inst.dst.reg.file==IR::RegisterFile::M0)
   inst.src[0]=Imm(35|(33u<<12));
  if(upper_outputs && inst.op==IR::Opcode::Export) {
   // Emit upper first, then poison lower: aliased staging must fail the render readback.
   std::array<uint32_t,4> sources {};
   for(uint32_t i=0;i<inst.src_count;++i)
    if(inst.src[i].kind==IR::OperandKind::Register && inst.src[i].reg.file==IR::RegisterFile::Vector)
     sources[i]=EmitValueLoad(&state,inst.src[i]);
   state.lane_half=1;
   for(uint32_t i=0;i<inst.src_count;++i) if(sources[i]!=0) EmitStoreU32(&state,inst.src[i],sources[i]);
   auto upper=inst;
   if(upper.export_info.kind==IR::ExportTargetKind::Primitive) upper.src[0]=Imm(32u|(33u<<10)|(34u<<20));
   upper.execution=IR::InstructionExecution::Vector;
   Require(EmitGuestInstruction(&state,upper));
   state.lane_half=0;
   if(inst.export_info.kind==IR::ExportTargetKind::Primitive) inst.src[0]=Imm(0x80000000u);
   else for(uint32_t i=0;i<inst.src_count;++i) inst.src[i]=Imm(i==3 ? 0x3f800000u : 0u);
  }
  EmitInstruction(&state,inst);
 }
 state.builder.AddFunction({OpReturn});state.builder.AddFunction({OpFunctionEnd});
 EmitNativeMeshEntryPoint(&state);
 const auto words=state.builder.Build();
 auto* file=std::fopen(upper_outputs ? "native_mesh_upper_outputs.spv" : "native_mesh_lane_backing.spv","wb");Require(file!=nullptr);
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
 if(argc==1){WriteLaneBackingFixture(p,info,resources);WriteLaneBackingFixture(p,info,resources,true);}
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
