#include "graphics/shader/recompiler/meshOutputProbe.h"
#include "graphics/host_gpu/renderer/meshSubgroupPolicy.h"
#include "graphics/host_gpu/renderer/singlePointIndex.h"
#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"
#include "common/assert.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/MergedMeshResources.h"
#include "graphics/shader/recompiler/ShaderInfoCollection.h"
#include <array>
#include <fstream>
#include <source_location>
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
void Require(bool ok,const std::string& error={},std::source_location where=std::source_location::current()){if(!ok){std::fprintf(stderr,"Mesh emitter test failed line=%u: %s\n",where.line(),error.c_str());std::exit(1);}}
// Exercise the production register allocator/load/store emitter without enabling public wave64.
// A render readback must distinguish upper/lower VGPR values while seeing a shared scalar write.
void WriteLaneBackingFixture(const IR::Program& program, const ShaderVertexInputInfo& info,
                             const IR::ResourceSnapshot& resources, bool upper_outputs=false, bool phased=false, bool point_push=false, bool buffer_reads=false) {
 using namespace Spirv::Emitter;
 EmitterState state;state.builder=Spirv::Builder(0x00010400u);
 auto mesh_info=info;
 auto fixture_program=program;auto fixture_resources=resources;
 if(point_push) {
  fixture_program.stage=ShaderType::Mesh;fixture_program.wave_size=64;
  fixture_program.user_data_base=8;
  fixture_program.bindings.user_data_registers={8};
  fixture_program.bindings.push_constant_size=24;
  fixture_program.bindings.mesh_point_input_dword=1;
  fixture_resources.user_data={0x87654321u};
 }
 if(buffer_reads) {
  IR::BufferResource buffer;buffer.packed_stride=16;buffer.max_byte_extent=4096;buffer.read=true;buffer.scalar=true;
  fixture_program.info.buffers.push_back(buffer);
  IR::DescriptorBinding binding;binding.kind=IR::DescriptorBindingKind::Buffers;binding.resources={0};binding.binding=0;
  fixture_program.bindings.descriptors.push_back(binding);
  IR::DescriptorValue descriptor;descriptor.dword_count=4;descriptor.dwords={0x40000000u,16u<<16,256,0};
  fixture_resources.buffers.push_back(descriptor);
 }
 mesh_info.mesh.lds_dwords=2*info.mesh.invocations;
 const uint32_t upper_base=2*info.mesh.invocations-32;
 if(upper_outputs){mesh_info.mesh.max_vertices=upper_base+3;mesh_info.mesh.max_primitives=upper_base+1;}
 state.program=&fixture_program;state.resources=&fixture_resources;state.vertex_input_info=&mesh_info;
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
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,46});
 CollectRegister(&state.registers,{IR::RegisterFile::Scalar,47});
 CollectRegister(&state.registers,SccRegister());
 CollectRegister(&state.registers,{IR::RegisterFile::Vector,17});
 for(uint32_t reg=18;reg<=20;++reg) CollectRegister(&state.registers,{IR::RegisterFile::Vector,reg});
 if(buffer_reads) {
  for(uint32_t reg=21;reg<=24;++reg) CollectRegister(&state.registers,{IR::RegisterFile::Vector,reg});
  for(uint32_t reg=48;reg<=63;++reg) CollectRegister(&state.registers,{IR::RegisterFile::Scalar,reg});
 }
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
 if(!point_push) EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,8),ConstantU32(&state,0x87654321u));
 state.lane_half=1;
 Require(point_push ? EmitMeshPointPushInputRegisters(&state,abi_assembly) : EmitMeshPointInputRegisters(&state,abi_assembly,abi_inputs));
 Require(state.lane_half==1);state.lane_half=0;
 auto fixture_push=[&](uint32_t dword) {
  const auto pointer=state.builder.AllocateId(),value=state.builder.AllocateId();
  state.builder.AddFunction({OpAccessChain,state.ptr_push_constant_uint,pointer,state.push_constant_variable,
   ConstantU32(&state,0),ConstantU32(&state,dword/4),ConstantU32(&state,dword%4)});
  state.builder.AddFunction({OpLoad,state.uint_type,value,pointer});return value;
 };
 const auto abi_count=point_push ? fixture_push(1) : ConstantU32(&state,1);
 const auto abi_first=point_push ? fixture_push(2) : ConstantU32(&state,25);
 const auto abi_instance=point_push ? fixture_push(3) : ConstantU32(&state,27);
 const auto abi_low_expected=EmitBinaryU32(&state,OpIAdd,ConstantU32(&state,0xabcdef01u),
  EmitBinaryU32(&state,OpISub,abi_first,ConstantU32(&state,25)));

 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,1}),0x12345678u);
 check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,8}),0x87654321u); // ES payload survives
 const auto abi_wave=EmitBinaryU32(&state,OpShiftRightLogical,index,ConstantU32(&state,5));
 const auto first_wave=state.builder.AllocateId();
 state.builder.AddFunction({OpIEqual,state.bool_type,first_wave,abi_wave,ConstantU32(&state,0)});
 const auto abi_s3=EmitBinaryU32(&state,OpBitwiseOr,
  EmitBinaryU32(&state,OpShiftLeftLogical,abi_wave,ConstantU32(&state,24)),
  EmitBinaryU32(&state,OpBitwiseOr,ConstantU32(&state,abi_assembly.guest_waves<<28),
   EmitSelectU32Value(&state,first_wave,EmitBinaryU32(&state,OpBitwiseOr,abi_count,EmitBinaryU32(&state,OpShiftLeftLogical,abi_count,ConstantU32(&state,8))),ConstantU32(&state,0))));
 auto abi_check=[&](uint32_t actual,uint32_t expected) {
  const auto equal=state.builder.AllocateId(),both=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,equal,actual,expected});
  state.builder.AddFunction({OpLogicalAnd,state.bool_type,both,passed,equal});passed=both;
 };
 abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,0}),abi_low_expected);
 abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,3}),abi_s3);
 for(uint32_t half=0;half<2;++half) {
  state.lane_half=half;
  // Independent mapping from the native index, not the helper under test.
  const auto guest=EmitBinaryU32(&state,OpIAdd,
   EmitBinaryU32(&state,OpIAdd,index,EmitBinaryU32(&state,OpBitwiseAnd,index,ConstantU32(&state,~31u))),
   ConstantU32(&state,half*32));
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,0}),EmitBinaryU32(&state,OpIMul,guest,ConstantU32(&state,4)));
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,1}),0);
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,5}),EmitBinaryU32(&state,OpIAdd,guest,abi_first));
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,8}),abi_instance);
 }
 state.lane_half=0;
 // Captured ES address chain: wave*64+lane, then dword-to-byte conversion in place.
 auto address_instruction=[&](IR::Opcode op,uint32_t dst,std::initializer_list<IR::Operand> sources) {
  IR::Instruction inst;inst.op=op;inst.execution=IR::InstructionExecution::Vector;
  inst.dst=Reg(IR::RegisterFile::Vector,dst);
  for(const auto& source:sources) inst.src[inst.src_count++]=source;
  return inst;
 };
 for(uint32_t half=0;half<2;++half) {
  state.lane_half=half;
  EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,18),EmitGuestSubgroupLocalInvocationId(&state));
 }
 state.lane_half=0;
 EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,28),abi_wave);
 auto address=address_instruction(IR::Opcode::ShiftLeftAddU32,18,
  {Reg(IR::RegisterFile::Scalar,28),Imm(6),Reg(IR::RegisterFile::Vector,18)});
 state.lane_half=1;Require(EmitGuestInstruction(&state,address));Require(state.lane_half==1);state.lane_half=0;
 auto scale=address_instruction(IR::Opcode::ShiftLeftLogicalU32,18,{Reg(IR::RegisterFile::Vector,18),Imm(2)});
 Require(EmitGuestInstruction(&state,scale));
 for(uint32_t half=0;half<2;++half) {
  state.lane_half=half;
  const auto expected=EmitBinaryU32(&state,OpIMul,
   EmitBinaryU32(&state,OpIAdd,index,EmitBinaryU32(&state,OpIAdd,
    EmitBinaryU32(&state,OpBitwiseAnd,index,ConstantU32(&state,~31u)),ConstantU32(&state,half*32))),ConstantU32(&state,4));
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,18}),expected);
  EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,19),EmitBinaryU32(&state,OpIAdd,expected,ConstantU32(&state,3)));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,20),ConstantU32(&state,half==0 ? 0x40000001u : 0x80000002u));
 }
 state.lane_half=0;
 auto align=address_instruction(IR::Opcode::BitwiseAndU32,19,{Imm(0xfffc),Reg(IR::RegisterFile::Vector,19)});
 Require(EmitGuestInstruction(&state,align));
 // Modulo32 shift count, u32 wrap and both source0/source2 aliases of the destination.
 auto overlap=address_instruction(IR::Opcode::ShiftLeftAddU32,20,
  {Reg(IR::RegisterFile::Vector,20),Imm(33),Reg(IR::RegisterFile::Vector,20)});
 Require(EmitGuestInstruction(&state,overlap));
 auto masked_shift=address_instruction(IR::Opcode::ShiftLeftLogicalU32,20,{Reg(IR::RegisterFile::Vector,20),Imm(34)});
 Require(EmitGuestInstruction(&state,masked_shift));
 for(uint32_t half=0;half<2;++half) {
  state.lane_half=half;
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,19}),EmitRegisterLoad(&state,{IR::RegisterFile::Vector,18}));
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,20}),half==0 ? 12u : 24u);
 }
 state.lane_half=0;
 // Unsupported forms must reject before changing registers or the active half.
 auto rejected=address;rejected.src_count=2;Require(!EmitGuestInstruction(&state,rejected));
 rejected=scale;rejected.dst=Reg(IR::RegisterFile::Scalar,28);Require(!EmitGuestInstruction(&state,rejected));
 rejected=align;rejected.dst2=Reg(IR::RegisterFile::Vcc,0);Require(!EmitGuestInstruction(&state,rejected));
 rejected=scale;rejected.src[0].sdwa_sel=0;rejected.src[0].sdwa_sext=true;Require(!EmitGuestInstruction(&state,rejected));
 rejected=address;rejected.src[0].dpp=true;rejected.src[0].dpp_ctrl=0x111;Require(!EmitGuestInstruction(&state,rejected));
 Require(state.lane_half==0 && state.instruction_register_values.empty());
 // Different sparse EXEC words preserve inactive lanes through each new opcode.
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,2));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,4));
 auto sparse=address_instruction(IR::Opcode::ShiftLeftAddU32,18,{Reg(IR::RegisterFile::Vector,18),Imm(1),Imm(5)});
 Require(EmitGuestInstruction(&state,sparse));
 scale.src[1]=Imm(1);Require(EmitGuestInstruction(&state,scale));
 auto sparse_align=address_instruction(IR::Opcode::BitwiseAndU32,18,{Imm(0xfffffffcu),Reg(IR::RegisterFile::Vector,18)});
 Require(EmitGuestInstruction(&state,sparse_align));
 for(uint32_t half=0;half<2;++half) {
  state.lane_half=half;
  const auto active=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,active,EmitSubgroupLocalInvocationId(&state),ConstantU32(&state,half==0?1u:2u)});
  const auto original=EmitRegisterLoad(&state,{IR::RegisterFile::Vector,19});
  const auto changed=EmitBinaryU32(&state,OpIAdd,EmitBinaryU32(&state,OpIMul,original,ConstantU32(&state,4)),ConstantU32(&state,8));
  abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,18}),EmitSelectU32Value(&state,active,changed,original));
 }
 state.lane_half=0;
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,0xffffffff));
 EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,0xffffffff));
 abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,28}),abi_wave);
 if(buffer_reads) {
  // Real SSBO reads: x1/x2/x3/x4 groups alias their index register on component zero.
  IR::Instruction load;load.op=IR::Opcode::BufferLoadDword;load.execution=IR::InstructionExecution::Vector;
  load.src_count=2;load.src[0]=Reg(IR::RegisterFile::Vector,21);load.src[1]=Imm(0);
  load.memory.kind=IR::ResourceKind::Buffer;load.memory.resource=0;load.memory.idxen=true;
  load.memory.data_bits=32;load.memory.data_dwords=1;
  for(uint32_t width=1;width<=4;++width) for(uint32_t repeat=0;repeat<2;++repeat) {
   for(uint32_t half=0;half<2;++half) {
    state.lane_half=half;
    const auto guest=EmitMeshGuestLocalInvocationIndex(&state);
    const auto record=repeat==0 ? guest : EmitBinaryU32(&state,OpISub,ConstantU32(&state,255),guest);
    EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,21),record);
   }
   state.lane_half=0;load.pc=0xd000+width*16;load.memory.component_count=width;
   for(uint32_t c=0;c<width;++c) {
    load.dst=Reg(IR::RegisterFile::Vector,21+c);load.memory.component_index=c;load.memory.offset=c*4;
    Require(EmitGuestInstruction(&state,load));
   }
   for(uint32_t half=0;half<2;++half) {
    state.lane_half=half;const auto guest=EmitMeshGuestLocalInvocationIndex(&state);
    const auto record=repeat==0 ? guest : EmitBinaryU32(&state,OpISub,ConstantU32(&state,255),guest);
    for(uint32_t c=0;c<width;++c)
     abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,21+c}),
      EmitBinaryU32(&state,OpIAdd,EmitBinaryU32(&state,OpIMul,record,ConstantU32(&state,4)),ConstantU32(&state,0x2000+c)));
   }
   state.lane_half=0;
  }
  // Scalar buffer x1/x2/x4/x8/x16 uses the same bound descriptor once per guest wave.
  IR::Instruction scalar;scalar.op=IR::Opcode::SBufferLoadDword;scalar.execution=IR::InstructionExecution::Scalar;
  scalar.src_count=1;scalar.src[0]=Imm(0);scalar.memory.kind=IR::ResourceKind::ScalarBuffer;
  scalar.memory.resource=0;scalar.memory.data_bits=32;scalar.memory.data_dwords=1;
  for(uint32_t width: {1u,2u,4u,8u,16u}) {
   scalar.memory.component_count=width;
   for(uint32_t c=0;c<width;++c) {
    scalar.dst=Reg(IR::RegisterFile::Scalar,48+c);scalar.memory.component_index=c;scalar.memory.offset=c*4;
    Require(EmitGuestInstruction(&state,scalar));check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,48+c}),0x2000+c);
   }
  }
  // Distinct EXEC words preserve inactive destinations; address overlap stays snapshot-safe.
  for(uint32_t half=0;half<2;++half) {
   state.lane_half=half;EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,21),EmitMeshGuestLocalInvocationIndex(&state));
   for(uint32_t c=1;c<4;++c) EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,21+c),ConstantU32(&state,0xdeadbeef));
  }
  state.lane_half=0;EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,2));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,4));
  load.pc=0xd080;load.memory.component_count=4;
  for(uint32_t c=0;c<4;++c) {
   load.dst=Reg(IR::RegisterFile::Vector,21+c);load.memory.component_index=c;load.memory.offset=c*4;
   Require(EmitGuestInstruction(&state,load));
  }
  for(uint32_t half=0;half<2;++half) {
   state.lane_half=half;const auto guest=EmitMeshGuestLocalInvocationIndex(&state),active=state.builder.AllocateId();
   state.builder.AddFunction({OpIEqual,state.bool_type,active,EmitSubgroupLocalInvocationId(&state),ConstantU32(&state,half==0?1u:2u)});
   for(uint32_t c=0;c<4;++c) {
    const auto expected=EmitBinaryU32(&state,OpIAdd,EmitBinaryU32(&state,OpIMul,guest,ConstantU32(&state,4)),ConstantU32(&state,0x2000+c));
    abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,21+c}),EmitSelectU32Value(&state,active,expected,c==0?guest:ConstantU32(&state,0xdeadbeef)));
   }
  }
  state.lane_half=0;EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,0xffffffff));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,0xffffffff));
  load.pc=0xd100;load.memory.component_count=1;load.memory.component_index=0;load.memory.offset=0;
  load.src[0]=Imm(256);load.dst=Reg(IR::RegisterFile::Vector,21);Require(EmitGuestInstruction(&state,load));
  for(uint32_t half=0;half<2;++half) {state.lane_half=half;check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,21}),0);}
  state.lane_half=0;
  auto rejected=load;rejected.memory.formatted=true;Require(!EmitGuestInstruction(&state,rejected));
  rejected=load;rejected.memory.component_count=5;Require(!EmitGuestInstruction(&state,rejected));
  rejected=load;rejected.src[0].dpp=true;Require(!EmitGuestInstruction(&state,rejected));
  Require(state.instruction_register_values.empty() && state.lane_half==0);
 }
 // Full-shader ALU families: distinct bank values, in-place destinations and sparse EXEC.
 {
  const auto old_low=EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,0});
  const auto old_high=EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,1});
  auto run=[&](IR::Opcode op,std::initializer_list<IR::Operand> sources) {
   IR::Instruction i;i.op=op;i.execution=IR::InstructionExecution::Vector;
   i.dst=Reg(IR::RegisterFile::Vector,20);for(const auto& s:sources)i.src[i.src_count++]=s;
   Require(EmitGuestInstruction(&state,i));
  };
  auto banks=[&](uint32_t low,uint32_t high) {
   for(uint32_t half=0;half<2;++half){state.lane_half=half;
    EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,20),ConstantU32(&state,half ? high : low));}
   state.lane_half=0;
  };
  auto expect=[&](uint32_t low,uint32_t high) {
   for(uint32_t half=0;half<2;++half){state.lane_half=half;check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,20}),half?high:low);}
   state.lane_half=0;
  };
  const auto v=Reg(IR::RegisterFile::Vector,20);
  banks(0x40000000,0x40800000);run(IR::Opcode::FMadF32,{v,Imm(0x40400000),Imm(0x3f800000)});expect(0x40e00000,0x41500000); // 7,13
  auto negative=v;negative.negate=true;
  run(IR::Opcode::FMulF32,{negative,Imm(0x40000000)});expect(0xc1600000,0xc1d00000); // -14,-26
  run(IR::Opcode::FSubF32,{v,Imm(0x3f800000)});expect(0xc1700000,0xc1d80000); // -15,-27
  banks(0x40000000,0x40800000);run(IR::Opcode::RcpF32,{v});expect(0x3f000000,0x3e800000);
  banks(0x40800000,0x41800000);run(IR::Opcode::InverseSqrtF32,{v});expect(0x3f000000,0x3e800000);
  banks(0,0);run(IR::Opcode::SinF32,{v});expect(0,0);
  banks(0xff000003,0xab000007);run(IR::Opcode::UMulU24U32,{v,Imm(0xcd000005)});expect(15,35);
  run(IR::Opcode::IMulU32,{v,Imm(0xffffffff)});expect(uint32_t(-15),uint32_t(-35));
  banks(1,2);run(IR::Opcode::ShiftLeftOrU32,{v,Imm(4),Imm(3)});expect(19,35);
  run(IR::Opcode::ShiftRightLogicalU32,{v,Imm(1)});expect(9,17);
  banks(0x00000001,0x00000002);run(IR::Opcode::BitReverseU32,{v});expect(0x80000000,0x40000000);
  banks(0xaaaa1234,0xbbbb5678);run(IR::Opcode::PackLowLowU16,{v,Imm(0xcccc9876)});expect(0x98761234,0x98765678);
  banks(1,2);run(IR::Opcode::BitwiseOr3U32,{Imm(4),Imm(8),v});expect(13,14);
  banks(7,13);run(IR::Opcode::UMaxU32,{v,Imm(10)});expect(10,13);
  for(uint32_t half=0;half<2;++half){state.lane_half=half;EmitStoreU32(&state,v,EmitSubgroupLocalInvocationId(&state));}
  state.lane_half=0;
  for(const auto op: {IR::Opcode::CompareMaskEqU32,IR::Opcode::CompareMaskNeU32,IR::Opcode::CompareMaskLtU32,IR::Opcode::CompareMaskGtU32}) {
   IR::Instruction cmp;cmp.op=op;cmp.execution=IR::InstructionExecution::Vector;
   cmp.dst=Reg(IR::RegisterFile::Vcc,0);cmp.src_count=2;cmp.src[0]=v;cmp.src[1]=Imm(1);
   Require(EmitGuestInstruction(&state,cmp));
   const uint32_t wanted=op==IR::Opcode::CompareMaskEqU32?2u:op==IR::Opcode::CompareMaskNeU32?0xfffffffdu:op==IR::Opcode::CompareMaskLtU32?1u:0xfffffffcu;
   check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,0}),wanted);check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,1}),wanted);
  }
  // VOPC may write an SGPR pair, including one of its own scalar sources.
  EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,46),ConstantU32(&state,2));
  IR::Instruction alias;alias.op=IR::Opcode::CompareGtU32;alias.execution=IR::InstructionExecution::Vector;
  alias.dst=Reg(IR::RegisterFile::Scalar,46);alias.src_count=2;alias.src[0]=alias.dst;alias.src[1]=v;
  Require(EmitGuestInstruction(&state,alias));check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,46}),3);
  check(EmitRegisterLoad(&state,{IR::RegisterFile::Scalar,47}),3);
  for(const auto op: {IR::Opcode::CompareNeU32,IR::Opcode::CompareLeU32,IR::Opcode::CompareLtI32}) {
   alias.op=op;alias.dst=Reg(IR::RegisterFile::Vcc,0);alias.src[0]=v;alias.src[1]=Imm(1);
   Require(EmitGuestInstruction(&state,alias));const uint32_t want=op==IR::Opcode::CompareNeU32?0xfffffffdu:op==IR::Opcode::CompareLeU32?3u:1u;
   check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,0}),want);check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,1}),want);
  }
  for(uint32_t byte=0;byte<3;++byte) {
   banks(0x12345678,0x9abcdef0);auto selected=v;selected.sdwa_sel=byte;
   run(IR::Opcode::ShiftLeftLogicalU32,{selected,Imm(2)});
   expect(((0x12345678u>>(byte*8))&255)*4,((0x9abcdef0u>>(byte*8))&255)*4);
  }
  banks(0x3f800000,0x40400000);
  IR::Instruction cmp;cmp.op=IR::Opcode::CompareMaskLtF32;cmp.execution=IR::InstructionExecution::Vector;
  cmp.dst=Reg(IR::RegisterFile::Vcc,0);cmp.src_count=2;cmp.src[0]=v;cmp.src[1]=Imm(0x40000000);
  Require(EmitGuestInstruction(&state,cmp));check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,0}),0xffffffff);check(EmitRegisterLoad(&state,{IR::RegisterFile::Vcc,1}),0);
  banks(0x40000000,0x40800000);
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,2));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,4));
  run(IR::Opcode::FMadF32,{v,Imm(0x40400000),Imm(0x3f800000)});
  for(uint32_t half=0;half<2;++half){state.lane_half=half;const auto active=state.builder.AllocateId();
   state.builder.AddFunction({OpIEqual,state.bool_type,active,EmitSubgroupLocalInvocationId(&state),ConstantU32(&state,half?2u:1u)});
   abi_check(EmitRegisterLoad(&state,{IR::RegisterFile::Vector,20}),EmitSelectU32Value(&state,active,
    ConstantU32(&state,half?0x41500000:0x40e00000),ConstantU32(&state,half?0x40800000:0x40000000)));}
  state.lane_half=0;
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,0),ConstantU32(&state,0xffffffff));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Exec,1),ConstantU32(&state,0xffffffff));
  EmitStoreU32(&state,Reg(IR::RegisterFile::Vcc,0),old_low);EmitStoreU32(&state,Reg(IR::RegisterFile::Vcc,1),old_high);
 }
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
 // S28 previously held the guest wave ID; the scalar-once check needs its own zero baseline.
 EmitStoreU32(&state,Reg(IR::RegisterFile::Scalar,28),ConstantU32(&state,0));
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
   EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,14),EmitRegisterLoad(&state,{IR::RegisterFile::Vector,19}));
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
 if(point_push) {
  const auto second=state.builder.AllocateId();
  state.builder.AddFunction({OpIEqual,state.bool_type,second,abi_first,ConstantU32(&state,41)});
  const auto color=EmitRegisterLoad(&state,value);
  EmitStoreU32(&state,Reg(IR::RegisterFile::Vector,17),EmitSelectU32Value(&state,second,color,ConstantU32(&state,0)));
  EmitStoreU32(&state,Reg(value.file,value.index),EmitSelectU32Value(&state,second,ConstantU32(&state,0),color));
 }
 for(const auto& original:program.blocks[0].instructions) {
  auto inst=original;
  if(inst.op==IR::Opcode::Export && inst.export_info.kind==IR::ExportTargetKind::Parameter) {
   if(inst.export_info.index==0) {inst.src[0]=Reg(value.file,value.index);if(point_push) inst.src[1]=Reg(IR::RegisterFile::Vector,17);}
   // Gate every color channel on the checks, including the green push-data case.
   if(inst.export_info.index==1)
    for(uint32_t i=0;i<inst.src_count;++i) inst.src[i]=Reg(shared.file,shared.index);
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
    state.builder.AddFunction({OpIEqual,state.bool_type,leader,index,ConstantU32(&state,(upper_base-32)/2)});
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
 auto* file=std::fopen(buffer_reads ? "native_mesh_buffer_reads.spv" : point_push ? "native_mesh_point_push.spv" : phased ? "native_mesh_phased_dispatcher.spv" : upper_outputs ? (info.mesh.invocations==64 ? "native_mesh_upper_outputs_multiwave.spv" : "native_mesh_upper_outputs.spv") : "native_mesh_lane_backing.spv","wb");Require(file!=nullptr);
 Require(std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size());
 Require(std::fclose(file)==0);
}
void WriteCapturedMesh(const char* es_path,const char* gs_path) {
 auto read=[](const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);Require(f.is_open());
  const auto n=f.tellg();Require(n>0 && n<=128*1024 && n%4==0);std::vector<uint32_t> w(size_t(n)/4);
  f.seekg(0);Require(bool(f.read(reinterpret_cast<char*>(w.data()),n)));return w;};
 const auto es=read(es_path),gs=read(gs_path);std::string error;std::vector<uint32_t> joined;
 Decoder::Program decoded;Require(Decoder::DecodeFusedProgram(es,gs,&joined,&decoded,&error),error);
 CFG::Graph graph;Require(CFG::BuildGraph(decoded,&graph,&error),error);
 IR::Program program;Require(IR::LowerProgram(decoded,graph,ShaderType::Mesh,64,&program,&error),error);
 program.dispatcher_fallback=true;Require(IR::BuildMergedMeshResourcePlan(&program,16,&error),error);
 IR::SrtRuntime runtime;std::array<uint32_t,16> userdata{};runtime.user_data=userdata;
 runtime.mesh_gs_user_data_address=0x123450000ull;runtime.shader_base=0x123400000ull;
 runtime.read_memory=[](void*,uint64_t addr,uint32_t* word){
  if(addr<0x123450000ull || addr>=0x123450040ull || addr%4) return false;
  const uint32_t offset=uint32_t(addr-0x123450000ull);
  const uint32_t descriptor[]={0x40000000u+(offset/16)*0x10000u,16u<<16,4096u,0u};
  *word=descriptor[(offset/4)%4];return true;};
 IR::ResourceSnapshot resources;Require(IR::MaterializeResources(program,runtime,&resources,&error),error);
 Require(IR::SpecializeResources(&program,resources,&error),error);
 MeshAssemblyPlan assembly;Require(PlanPointMeshAssembly(3,24,72,216,64,&assembly));
 ShaderVertexInputInfo info{};info.mesh={216,210,128,32,8192};
 IR::ShaderInfoOptions opts;opts.vertex=&info;Require(IR::CollectShaderInfo(&program,opts,&error),error);
 IR::BindingLayoutOptions bindings;bindings.mesh_point_inputs=true;Require(IR::AllocateBindings(&program,bindings,&error),error);
 // Only compile here: synthetic descriptor contents cannot prove a game framebuffer.
 std::vector<uint32_t> words;error.clear();
 const bool emitted=Spirv::EmitMergedPointMeshProgram(program,resources,info,assembly,&words,&error);
 std::printf("[ASTRO-FULL-MESH] decoded=%zu blocks=%zu buffers=%zu emitted=%u words=%zu resources=synthetic GPU=not-executed error=%s\n",
  decoded.instructions.size(),program.blocks.size(),program.info.buffers.size(),emitted,words.size(),error.c_str());
 Require(emitted,error);
 auto* f=std::fopen("astro_full_mesh.spv","wb");Require(f!=nullptr);
 Require(std::fwrite(words.data(),4,words.size(),f)==words.size());Require(std::fclose(f)==0);
}
int main(int argc,char** argv){
 {
  const std::array<uint8_t,5> bytes{0xaa,0x34,0x12,0x78,0x56};
  uint32_t vertex=99;
  Require(DecodeSinglePointIndex(bytes.data()+1,0,-3,&vertex) && vertex==0x1231);
  Require(DecodeSinglePointIndex(bytes.data()+1,1,5,&vertex) && vertex==0x56781239);
  Require(DecodeSinglePointIndex(bytes.data()+1,2,-0x35,&vertex) && vertex==0xffffffff);
  Require(!DecodeSinglePointIndex(bytes.data(),3,0,&vertex) && vertex==0xffffffff);
  Require(!DecodeSinglePointIndex(nullptr,0,0,&vertex) && vertex==0xffffffff);
  Require(!DecodeSinglePointIndex(bytes.data(),0,0,nullptr));
 }

 {
  auto p=SelectMeshWave32Policy(true,true,16,32,32);
  Require(p.supported && p.required_size==32);
  p=SelectMeshWave32Policy(true,true,16,32,16);Require(p.supported && p.required_size==32);
  p=SelectMeshWave32Policy(false,true,16,32,32);Require(p.supported && p.required_size==0);
  p=SelectMeshWave32Policy(false,true,16,32,16);Require(!p.supported);
  p=SelectMeshWave32Policy(true,false,16,32,16);Require(!p.supported);
  p=SelectMeshWave32Policy(true,true,64,64,64);Require(!p.supported);
  p=SelectMeshWave32Policy(true,true,32,64,64);Require(p.supported && p.required_size==32);
 }
 Require(argc==1 || argc==2 || argc==3);
 if(argc==3){WriteCapturedMesh(argv[1],argv[2]);return 0;}
 {
  IR::Program probe_plan;probe_plan.stage=ShaderType::Mesh;probe_plan.shader_info_complete=true;
  IR::BindingLayoutOptions probe_options;probe_options.mesh_output_probe=true;std::string e;
  Require(IR::AllocateBindings(&probe_plan,probe_options,&e),e);
  Require(IR::FindBinding(probe_plan.bindings,IR::DescriptorBindingKind::GuestAddressFault)!=nullptr);
  Require(probe_plan.bindings.mesh_output_probe);
  Require(IR::FindBinding(probe_plan.bindings,IR::DescriptorBindingKind::GuestAddressTable)==nullptr);
  IR::Program rejected;rejected.stage=ShaderType::Vertex;rejected.shader_info_complete=true;
  Require(!IR::AllocateBindings(&rejected,probe_options,&e));Require(!rejected.binding_layout_complete);
 }
 {
  IR::Program plan;plan.stage=ShaderType::Mesh;plan.shader_info_complete=true;
  IR::BindingLayoutOptions options;options.mesh_point_inputs=true;
  std::string error;Require(IR::AllocateBindings(&plan,options,&error),error);
  Require(plan.bindings.mesh_point_input_dword==0 && plan.bindings.push_constant_size==20);
  const auto layout=plan.bindings;
  IR::ResourceSnapshot snapshot;
  IR::MeshPointGroupPayload a{1,25,27,0xabcdef01,0x12345678},b{2,41,43,0xabcdef11,0x12345678};
  std::vector<uint32_t> bytes;
  Require(IR::BuildPushConstantPayload(plan,snapshot,&a,&bytes,&error),error);
  Require(bytes==std::vector<uint32_t>({1,25,27,0xabcdef01,0x12345678}));
  Require(IR::BuildPushConstantPayload(plan,snapshot,&b,&bytes,&error),error);
  Require(bytes==std::vector<uint32_t>({2,41,43,0xabcdef11,0x12345678}) && plan.bindings==layout);
  const auto saved=bytes;a.vertex_count=65;
  Require(!IR::BuildPushConstantPayload(plan,snapshot,&a,&bytes,&error));Require(bytes==saved);
  Require(!IR::BuildPushConstantPayload(plan,snapshot,nullptr,&bytes,&error));
  plan.bindings.user_data_registers={8};plan.user_data_base=8;snapshot.user_data={0x87654321};
  plan.bindings.mesh_point_input_dword=1;plan.bindings.push_constant_size=24;a.vertex_count=1;
  Require(IR::BuildPushConstantPayload(plan,snapshot,&a,&bytes,&error));
  Require(bytes.front()==0x87654321 && bytes[1]==1 && bytes[2]==25);
  plan.bindings.mesh_point_input_dword=0; // overlapping compact ES and draw data rejected
  Require(!IR::BuildPushConstantPayload(plan,snapshot,&a,&bytes,&error));
  IR::DescriptorBinding storage;storage.kind=IR::DescriptorBindingKind::UserData;
  plan.bindings.descriptors.push_back(storage);plan.bindings.push_constant_size=20;
  Require(IR::BuildPushConstantPayload(plan,snapshot,&a,&bytes,&error));Require(bytes.size()==5 && bytes[0]==1);
  auto fresh=[](){IR::Program p;p.stage=ShaderType::Mesh;p.shader_info_complete=true;return p;};
  auto limited=fresh();options.push_constant_offset=112;
  Require(!IR::AllocateBindings(&limited,options,&error));Require(!limited.binding_layout_complete);
  limited=fresh();options.push_constant_offset=108;Require(IR::AllocateBindings(&limited,options,&error));
  Require(limited.bindings.push_constant_size==20 && limited.bindings.push_constant_offset==108);
  limited=fresh();limited.stage=ShaderType::Vertex;Require(!IR::AllocateBindings(&limited,options,&error));
  limited=fresh();limited.user_data_base=8;limited.blocks.resize(1);
  IR::ScalarValue user;user.op=IR::ScalarValueOp::UserData;user.imm=8;
  limited.provenance.values.resize(2);limited.provenance.values.push_back(user);
  IR::Instruction source;source.src_count=1;source.scalar_sources[0]=2;
  limited.blocks[0].instructions.push_back(source);
  options.push_constant_offset=0;options.max_push_dwords=5;
  Require(IR::AllocateBindings(&limited,options,&error));
  Require(limited.bindings.user_data_registers==std::vector<uint32_t>({8}));
  Require(IR::FindBinding(limited.bindings,IR::DescriptorBindingKind::UserData)!=nullptr);
  Require(limited.bindings.push_constant_size==20 && limited.bindings.mesh_point_input_dword==0);
  Require(IR::BuildPushConstantPayload(limited,snapshot,&b,&bytes,&error));Require(bytes[1]==41);
  std::puts("[MESH-POINT-PUSH] payload updates layout preservation bounds overlap=passed GPU=not-executed");
 }
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
 if(MeshOutputProbeEnabled()) {
  auto probe_program=p;IR::DescriptorBinding diagnostic;
  diagnostic.kind=IR::DescriptorBindingKind::GuestAddressFault;diagnostic.binding=0;
  probe_program.bindings.descriptors.push_back(diagnostic);
  Require(!Spirv::EmitProgram(probe_program,resources,&info,nullptr,nullptr,&words,&error));
  probe_program.bindings.mesh_output_probe=true;
  // Exercise the exact diagnostic selector with known positive input before the real CMPX.
  std::vector<IR::Instruction> gate;
  auto add_gate=[&](IR::Opcode op,IR::Operand dst,std::initializer_list<IR::Operand> src,uint32_t pc) {
   IR::Instruction i;i.op=op;i.dst=dst;i.pc=pc;
   for(const auto& operand:src)i.src[i.src_count++]=operand;gate.push_back(i);
  };
  add_gate(IR::Opcode::CompareMaskGtU32,Reg(IR::RegisterFile::Exec,0),{Imm(31),vector(5)},0xe0);
  add_gate(IR::Opcode::CompareEqU32,mask,{vector(5),Imm(30)},0xe4);
  add_gate(IR::Opcode::SelectMaskU32,vector(8),{mask,Imm(0),Imm(0x3f800000)},0xf8);
  add_gate(IR::Opcode::MoveU32,vector(9),{Imm(7)},0xfc);
  for(uint32_t i=0;i<4;++i)add_gate(IR::Opcode::MoveU32,Reg(IR::RegisterFile::Scalar,8+i),{Imm(0x11111111u*(i+1))},0xfc);
  add_gate(IR::Opcode::CompareMaskLtF32,Reg(IR::RegisterFile::Exec,0),{Imm(0),vector(8)},0x104);
  probe_program.blocks[0].instructions.insert(probe_program.blocks[0].instructions.begin(),gate.begin(),gate.end());
  std::vector<uint32_t> probe_words;
  Require(Spirv::EmitProgram(probe_program,resources,&info,nullptr,nullptr,&probe_words,&error),error);
  auto* probe_file=std::fopen("native_mesh_output_probe.spv","wb");Require(probe_file!=nullptr);
  Require(std::fwrite(probe_words.data(),4,probe_words.size(),probe_file)==probe_words.size());
  Require(std::fclose(probe_file)==0);
  // Same trace ABI, now with the dynamic instance in the five-word point payload.
  probe_program.wave_size=64;probe_program.mesh_gs_user_data=true;probe_program.user_data_base=8;
  probe_program.dispatcher_fallback=true;
  probe_program.bindings.mesh_point_input_dword=0;
  probe_program.bindings.push_constant_size=20;
  for(auto& i:probe_program.blocks[0].instructions) i.execution=(i.op==IR::Opcode::Sendmsg ||
   (i.dst.kind==IR::OperandKind::Register && i.dst.reg.file==IR::RegisterFile::M0)) ? IR::InstructionExecution::Scalar : IR::InstructionExecution::Vector;
  auto point_probe_info=info;point_probe_info.mesh.invocations=32;
  MeshAssemblyPlan point_probe_assembly;Require(PlanPointMeshAssembly(1,1,3,3,64,&point_probe_assembly));
  Require(Spirv::EmitMergedPointMeshProgram(probe_program,resources,point_probe_info,point_probe_assembly,&probe_words,&error),error);
  auto* point_probe_file=std::fopen("native_mesh_point_output_probe.spv","wb");Require(point_probe_file!=nullptr);
  Require(std::fwrite(probe_words.data(),4,probe_words.size(),point_probe_file)==probe_words.size());
  Require(std::fclose(point_probe_file)==0);

 }
 Require(Spirv::EmitProgram(p,resources,&info,nullptr,nullptr,&words,&error),error);
 auto* file=std::fopen(argc==2 ? "native_mesh_emitter_multiwave.spv" : "native_mesh_emitter.spv","wb");Require(file!=nullptr);
 Require(std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size());Require(std::fclose(file)==0);
 if(argc==1){WriteLaneBackingFixture(p,info,resources);WriteLaneBackingFixture(p,info,resources,true);}
 else {WriteLaneBackingFixture(p,info,resources,true);WriteLaneBackingFixture(p,info,resources,false,true);auto point_info=info;point_info.mesh.invocations=128;WriteLaneBackingFixture(p,point_info,resources,true,false,true);WriteLaneBackingFixture(p,point_info,resources,true,false,true,true);}
 {
  auto merged=p;merged.wave_size=64;merged.mesh_gs_user_data=true;merged.user_data_base=8;
  merged.dispatcher_fallback=true;merged.bindings.mesh_point_input_dword=0;merged.bindings.push_constant_size=20;
  for(auto& i:merged.blocks[0].instructions) i.execution=(i.op==IR::Opcode::Sendmsg ||
   (i.dst.kind==IR::OperandKind::Register && i.dst.reg.file==IR::RegisterFile::M0)) ? IR::InstructionExecution::Scalar : IR::InstructionExecution::Vector;
  auto merged_info=info;merged_info.mesh.invocations=32;
  MeshAssemblyPlan assembly;Require(PlanPointMeshAssembly(1,1,3,3,64,&assembly));
  Require(Spirv::EmitMergedPointMeshProgram(merged,resources,merged_info,assembly,&words,&error),error);
  auto* f=std::fopen("native_mesh_full_adapter.spv","wb");Require(f!=nullptr);
  Require(std::fwrite(words.data(),4,words.size(),f)==words.size());Require(std::fclose(f)==0);
 }
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
