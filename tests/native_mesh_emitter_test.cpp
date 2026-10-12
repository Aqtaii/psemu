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
                             const IR::ResourceSnapshot& resources, bool upper_outputs=false, bool phased=false) {
 using namespace Spirv::Emitter;
 EmitterState state;state.builder=Spirv::Builder(0x00010400u);
 auto mesh_info=info;
 mesh_info.mesh.lds_dwords=2*info.mesh.invocations;
 const uint32_t upper_base=info.mesh.invocations==64 ? 96u : 32u;
 if(upper_outputs){mesh_info.mesh.max_vertices=upper_base+3;mesh_info.mesh.max_primitives=upper_base+1;}
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
 CollectRegister(&state.registers,{IR::RegisterFile::Vector,13});
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,28});
 for(uint32_t reg=14;reg<=16;++reg) CollectRegister(&state.registers,{IR::RegisterFile::Vector,reg});
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,29});
 CollectRegister(&state.registers,SccRegister());
 for(uint32_t reg: {0u,1u,3u,8u}) CollectRegister(&state.registers,{IR::RegisterFile::Scalar,reg});
 for(uint32_t reg: {0u,1u,5u,8u}) CollectRegister(&state.registers,{IR::RegisterFile::Vector,reg});
 CopyProgramInputsAndOutputs(&state,program);
 state.needs_subgroup_ballot=ProgramNeedsSubgroupBallot(program);
 state.needs_subgroup_shuffle=true;
 state.needs_subgroup_local_invocation_id=true;
 AllocateInputVariables(&state);AllocateOutputVariables(&state);
 AllocateDescriptorVariables(&state);EmitHeaderAndTypes(&state);AllocateRegisterVariables(&state);
 if(phased) {
  Require(info.mesh.invocations==64 && !upper_outputs);
  IR::Program dispatcher;dispatcher.dispatcher_fallback=true;
  AllocateDispatcherState(&state,dispatcher);
  state.mesh_phase_done_variable=state.builder.AllocateId();
 }

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
 state.builder.AddFunction({OpStore,PointerForRegister(state,{IR::RegisterFile::Vector,5}),EmitSubgroupLocalInvocationId(&state)});
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
 // The merged ABI adapter must initialize both banks, including the later guest wave.
 MeshAssemblyPlan abi_assembly;
 Require(PlanPointMeshAssembly(3,24,3,info.mesh.invocations*2,64,&abi_assembly));
 const MeshPointGroupInputIds abi_inputs {ConstantU32(&state,1),ConstantU32(&state,25),ConstantU32(&state,27),
  ConstantU32(&state,0xabcdef01u),ConstantU32(&state,0x12345678u)};
 EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,8),ConstantU32(&state,0x87654321u));
 state.lane_half=1;Require(EmitMeshPointInputRegisters(&state,abi_assembly,abi_inputs));
 Require(state.lane_half==1);state.lane_half=0;
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,0}),0xabcdef01u);
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,1}),0x12345678u);
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,8}),0x87654321u); // ES payload survives
 const auto abi_wave=EmitBinaryU32(&state,OpShiftRightLogical,index,ConstantU32(&state,5));
 const auto first_wave=state.builder.AllocateId();
 state.builder.AddFunction({OpIEqual,state.bool_type,first_wave,abi_wave,ConstantU32(&state,0)});
 const auto abi_s3=EmitBinaryU32(&state,OpBitwiseOr,
  EmitBinaryU32(&state,OpShiftLeftLogical,abi_wave,ConstantU32(&state,24)),
  EmitBinaryU32(&state,OpBitwiseOr,ConstantU32(&state,abi_assembly.guest_waves<<28),
   EmitSelectU32Value(&state,first_wave,ConstantU32(&state,0x101),ConstantU32(&state,0))));
 auto abi_check=[&](uint32_t actual,uint32_t expected) {
  const auto equal=state.builder.AllocateId(),both=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,equal,actual,expected});
  state.builder.AddFunction({OpLogicalAnd,state.bool_type,both,passed,equal});passed=both;
 };
 abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,3}),abi_s3);
 for(uint32_t half=0;half<2;++half) {
  state.lane_half=half;
  // Independent mapping from the native index, not the helper under test.
  const auto guest=EmitBinaryU32(&state,OpIAdd,
   EmitBinaryU32(&state,OpIAdd,index,EmitBinaryU32(&state,OpBitwiseAnd,index,ConstantU32(&state,~31u))),
   ConstantU32(&state,half*32));
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,0}),EmitBinaryU32(&state,OpIMul,guest,ConstantU32(&state,4)));
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,1}),0);
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,5}),EmitBinaryU32(&state,OpIAdd,guest,ConstantU32(&state,25)));
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,8}),27);
 }
 state.lane_half=0;
 // Restore the triangle fixture's local vertex ID after checking the merged entry ABI.
 EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,5),EmitSubgroupLocalInvocationId(&state));
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
 // READFIRSTLANE runs once and selects logical lanes across both banks.
 IR::Instruction first;first.op=IR::Opcode::ReadFirstLaneU32;
 first.execution=IR::InstructionExecution::Scalar;first.src_count=1;
 first.src[0]=Reg(IR::RegisterFile::Vector,10);first.dst=Reg(IR::RegisterFile::Scalar,27);
 auto first_check=[&](uint32_t low_mask,uint32_t high_mask,uint32_t expected){
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,low_mask));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,high_mask));
  state.lane_half=1;
  Require(EmitGuestInstruction(&state,first));
  Require(state.lane_half==1 && state.instruction_register_values.empty());
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,27}),expected);
  state.lane_half=0;
 };
 first_check(2,4,7); // lowest active lower lane wins
 first_check(0,4,9); // upper-only EXEC must read guest34
 first_check(0,0,0); // empty EXEC reads guest0
 first.src[0]=Reg(value.file,value.index);
 first_check(0x80000000u,0x80000000u,0x3f800000u); // lower31 holds sum1
 first_check(0,0x80000000u,0x3f400000u); // upper63 remains0.75
 first.execution=IR::InstructionExecution::Vector;
 Require(!EmitGuestInstruction(&state,first));
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
 // DPP row shifts must honor guest rows2/3 and snapshot overlapping sources.
 for(uint32_t half=0;half<2;++half){
  state.lane_half=half;
  EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,13),EmitGuestSubgroupLocalInvocationId(&state));
 }
 state.lane_half=0;
 IR::Instruction dpp;dpp.op=IR::Opcode::IAddU32;dpp.execution=IR::InstructionExecution::Vector;
 dpp.src_count=2;dpp.dst=Reg(IR::RegisterFile::Vector,13);
 dpp.dst.dpp=true;dpp.dst.dpp_ctrl=0x111;dpp.dst.dpp_row_mask=0xc;
 dpp.src[0]=dpp.dst;dpp.src[1]=Imm(3);
 Require(EmitGuestInstruction(&state,dpp));
 read_check(13,1,1);read_check(13,17,17); // lower rows masked out
 read_check(13,32,32);read_check(13,48,48); // invalid row predecessor preserves destination
 read_check(13,33,35);read_check(13,63,65);
 dpp.dst.dpp_ctrl=0x112;dpp.dst.dpp_row_mask=0xf;dpp.dst.dpp_bank_mask=2;
 dpp.src[0]=dpp.dst;
 Require(EmitGuestInstruction(&state,dpp));
 read_check(13,4,5);read_check(13,20,21);read_check(13,36,39);
 read_check(13,33,35); // bank0 masked out, upper previous value survives
 dpp.dst.dpp_ctrl=0x130;dpp.src[0]=dpp.dst;
 Require(!EmitGuestInstruction(&state,dpp)); // cross-row controls still unsupported
 // Two guest halves share LDS, including across the second native subgroup.
 const uint32_t logical_count=2*info.mesh.invocations;
 auto init_lds_registers=[&](uint32_t bias){
  for(uint32_t half=0;half<2;++half){
   state.lane_half=half;
   const auto guest=EmitMeshGuestLocalInvocationIndex(&state);
   EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,14),EmitBinaryU32(&state,OpShiftLeftLogical,guest,ConstantU32(&state,2)));
   EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,15),EmitBinaryU32(&state,OpIAdd,guest,ConstantU32(&state,bias)));
  }
  state.lane_half=0;
 };
 IR::Instruction ds_write;ds_write.op=IR::Opcode::DsWriteB32;
 ds_write.execution=IR::InstructionExecution::Vector;ds_write.memory.kind=IR::ResourceKind::Lds;
 ds_write.src_count=2;ds_write.src[0]=Reg(IR::RegisterFile::Vector,15);ds_write.src[1]=Reg(IR::RegisterFile::Vector,14);
 IR::Instruction ds_read;ds_read.op=IR::Opcode::DsReadB32;
 ds_read.execution=IR::InstructionExecution::Vector;ds_read.memory.kind=IR::ResourceKind::Lds;
 ds_read.src_count=1;ds_read.src[0]=Reg(IR::RegisterFile::Vector,14);ds_read.dst=ds_read.src[0];
 IR::Instruction barrier;barrier.op=IR::Opcode::Barrier;
 auto dynamic_check=[&](uint32_t actual,uint32_t expected){
  const auto equal=state.builder.AllocateId();
  const auto both=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,equal,actual,expected});
  state.builder.AddFunction({OpLogicalAnd,state.bool_type,both,passed,equal});passed=both;
 };
 init_lds_registers(1000);Require(EmitGuestInstruction(&state,ds_write));
 // Only this straight-line fixture calls a known-uniform barrier. Guest CFG remains gated.
 EmitBarrier(&state,barrier);
 for(uint32_t half=0;half<2;++half){
  state.lane_half=half;
  const auto partner=EmitBinaryU32(&state,OpISub,ConstantU32(&state,logical_count-1),EmitMeshGuestLocalInvocationIndex(&state));
  EmitStoreU32(&state,ds_read.src[0],EmitBinaryU32(&state,OpShiftLeftLogical,partner,ConstantU32(&state,2)));
 }
 state.lane_half=0;Require(EmitGuestInstruction(&state,ds_read));
 for(uint32_t half=0;half<2;++half){
  state.lane_half=half;
  const auto partner=EmitBinaryU32(&state,OpISub,ConstantU32(&state,logical_count-1),EmitMeshGuestLocalInvocationIndex(&state));
  dynamic_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,14}),EmitBinaryU32(&state,OpIAdd,partner,ConstantU32(&state,1000)));
 }
 state.lane_half=0;
 EmitBarrier(&state,barrier); // All reversed reads must finish before sparse overwrites.
 // Sparse EXEC writes must preserve all other initialized shared words.
 init_lds_registers(2000);
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,2));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,4));
 Require(EmitGuestInstruction(&state,ds_write));EmitBarrier(&state,barrier);
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,0xffffffff));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,0xffffffff));
 ds_read.dst=Reg(IR::RegisterFile::Vector,16);Require(EmitGuestInstruction(&state,ds_read));
 for(uint32_t half=0;half<2;++half){
  state.lane_half=half;
  const auto lane=EmitGuestSubgroupLocalInvocationId(&state);
  const auto hit=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,hit,lane,ConstantU32(&state,half==0 ? 1u : 34u)});
  const auto bias=EmitSelectU32Value(&state,hit,ConstantU32(&state,2000),ConstantU32(&state,1000));
  dynamic_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,16}),EmitBinaryU32(&state,OpIAdd,EmitMeshGuestLocalInvocationIndex(&state),bias));
 }
 state.lane_half=0;
 // Out-of-range loads return zero; writes cannot corrupt the live allocation or outputs.
 ds_read.src[0]=Imm(logical_count*4);Require(EmitGuestInstruction(&state,ds_read));
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,16}),0);
 state.lane_half=1;check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,16}),0);state.lane_half=0;
 ds_write.src[1]=Imm(logical_count*4);Require(EmitGuestInstruction(&state,ds_write));
 ds_write.memory.kind=IR::ResourceKind::Gds;Require(!EmitGuestInstruction(&state,ds_write));
 ds_read.memory.kind=IR::ResourceKind::Gds;Require(!EmitGuestInstruction(&state,ds_read));
 paired.execution=IR::InstructionExecution::Unknown;Require(!EmitGuestInstruction(&state,paired));
 paired.execution=IR::InstructionExecution::Scalar;paired.op=IR::Opcode::Barrier;
 Require(!EmitGuestInstruction(&state,paired));
 paired.execution=IR::InstructionExecution::Vector;paired.op=IR::Opcode::DsWriteAddtidB32;
 Require(!EmitGuestInstruction(&state,paired));
 Require(state.instruction_register_values.empty() && state.lane_half==0);
 if(phased) {
  EmitBarrier(&state,barrier); // Complete earlier LDS checks before reusing their shared words.
  init_lds_registers(1000);
  for(uint32_t half=0;half<2;++half) {
   state.lane_half=half;
   const auto partner=EmitBinaryU32(&state,OpISub,ConstantU32(&state,logical_count-1),EmitMeshGuestLocalInvocationIndex(&state));
   EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,16),EmitBinaryU32(&state,OpShiftLeftLogical,partner,ConstantU32(&state,2)));
  }
  state.lane_half=0;
  EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,28),EmitBinaryU32(&state,OpShiftRightLogical,index,ConstantU32(&state,5)));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,29),ConstantU32(&state,0));
  IR::Program flow;flow.stage=ShaderType::Mesh;flow.wave_size=64;flow.dispatcher_fallback=true;
  flow.blocks.resize(6);for(uint32_t i=0;i<6;++i) flow.blocks[i].id=i;
  auto branch=[&](uint32_t from,uint32_t to){flow.blocks[from].terminator.kind=CFG::TerminatorKind::Branch;flow.blocks[from].terminator.true_block=to;};
  auto instruction=[&](uint32_t block,IR::Opcode op,IR::InstructionExecution execution,IR::Operand dst,std::initializer_list<IR::Operand> src){
   IR::Instruction inst;inst.op=op;inst.execution=execution;inst.dst=dst;
   for(const auto& operand:src) inst.src[inst.src_count++]=operand;
   flow.blocks[block].instructions.push_back(inst);
  };
  const auto counter=Reg(IR::RegisterFile::Scalar,29);
  instruction(0,IR::Opcode::CompareEqU32,IR::InstructionExecution::Scalar,SccOperand(),{Reg(IR::RegisterFile::Scalar,28),Imm(0)});
  flow.blocks[0].terminator.kind=CFG::TerminatorKind::ConditionalBranch;
  flow.blocks[0].terminator.condition=CFG::BranchCondition::SccNonZero;
  flow.blocks[0].terminator.true_block=1;flow.blocks[0].terminator.false_block=2;
  branch(1,3);branch(2,4);branch(4,3);branch(3,5);
  instruction(2,IR::Opcode::IAddU32,IR::InstructionExecution::Scalar,counter,{counter,Imm(1)});
  instruction(4,IR::Opcode::IAddU32,IR::InstructionExecution::Scalar,counter,{counter,Imm(1)});
  instruction(3,IR::Opcode::IAddU32,IR::InstructionExecution::Vector,Reg(IR::RegisterFile::Vector,15),{Reg(IR::RegisterFile::Vector,15),counter});
  auto write=ds_write;write.memory.kind=IR::ResourceKind::Lds;write.src[1]=Reg(IR::RegisterFile::Vector,14);
  flow.blocks[3].instructions.push_back(write);
  auto sync=barrier;sync.execution=IR::InstructionExecution::Scalar;sync.pc=100;flow.blocks[3].instructions.push_back(sync);
  auto load=ds_read;load.memory.kind=IR::ResourceKind::Lds;load.dst=Reg(IR::RegisterFile::Vector,16);load.src[0]=load.dst;
  flow.blocks[3].instructions.push_back(load);
  sync.pc=104;flow.blocks[3].instructions.push_back(sync);
  instruction(3,IR::Opcode::IAddU32,IR::InstructionExecution::Scalar,counter,{counter,Imm(1)});
  Require(EmitMeshPhasedDispatcher(&state,flow));
  for(uint32_t half=0;half<2;++half) {
   state.lane_half=half;
   const auto partner=EmitBinaryU32(&state,OpISub,ConstantU32(&state,logical_count-1),EmitMeshGuestLocalInvocationIndex(&state));
   const auto other_group=state.builder.AllocateId();
   state.builder.AddFunction({OpUGreaterThanEqual,state.bool_type,other_group,partner,ConstantU32(&state,64)});
   const auto bias=EmitSelectU32Value(&state,other_group,ConstantU32(&state,1002),ConstantU32(&state,1000));
   dynamic_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,16}),EmitBinaryU32(&state,OpIAdd,partner,bias));
  }
  state.lane_half=0;
  const auto long_path=state.builder.AllocateId();
  state.builder.AddFunction({OpUGreaterThanEqual,state.bool_type,long_path,index,ConstantU32(&state,32)});
  dynamic_check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,29}),EmitSelectU32Value(&state,long_path,ConstantU32(&state,3),ConstantU32(&state,1)));
 }
 EmitStoreU32(&state,Reg(shared.file,shared.index),EmitSelectU32Value(&state,passed,ConstantU32(&state,0x3f800000),ConstantU32(&state,0)));
 for(const auto& original:program.blocks[0].instructions) {
  auto inst=original;
  if(inst.op==IR::Opcode::Export && inst.export_info.kind==IR::ExportTargetKind::Parameter) {
   if(inst.export_info.index==0) inst.src[0]=Reg(value.file,value.index);
   if(inst.export_info.index==1) inst.src[0]=Reg(shared.file,shared.index);
  }
  if(upper_outputs && inst.op==IR::Opcode::MoveU32 && inst.dst.kind==IR::OperandKind::Register && inst.dst.reg.file==IR::RegisterFile::M0)
   inst.src[0]=Imm(mesh_info.mesh.max_vertices|(mesh_info.mesh.max_primitives<<12));
  if(upper_outputs && inst.op==IR::Opcode::Export) {
   // Emit upper first, then poison lower: aliased staging must fail the render readback.
   std::array<uint32_t,4> sources {};
   for(uint32_t i=0;i<inst.src_count;++i)
    if(inst.src[i].kind==IR::OperandKind::Register && inst.src[i].reg.file==IR::RegisterFile::Vector)
     sources[i]=EmitValueLoad(&state,inst.src[i]);
   state.lane_half=1;
   for(uint32_t i=0;i<inst.src_count;++i) if(sources[i]!=0) EmitStoreU32(&state,inst.src[i],sources[i]);
   auto upper=inst;
   if(upper.export_info.kind==IR::ExportTargetKind::Primitive) {
    const auto leader=state.builder.AllocateId();
    state.builder.AddFunction({OpIEqual,state.bool_type,leader,index,ConstantU32(&state,upper_base==96 ? 32u : 0u)});
    const auto packed=upper_base|((upper_base+1)<<10)|((upper_base+2)<<20);
    EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,13),EmitSelectU32Value(&state,leader,ConstantU32(&state,packed),ConstantU32(&state,0x80000000u)));
    upper.src[0]=Reg(IR::RegisterFile::Vector,13);
   }
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
 auto* file=std::fopen(phased ? "native_mesh_phased_dispatcher.spv" : upper_outputs ? (info.mesh.invocations==64 ? "native_mesh_upper_outputs_multiwave.spv" : "native_mesh_upper_outputs.spv") : "native_mesh_lane_backing.spv","wb");Require(file!=nullptr);
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
 else {WriteLaneBackingFixture(p,info,resources,true);WriteLaneBackingFixture(p,info,resources,false,true);}
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
