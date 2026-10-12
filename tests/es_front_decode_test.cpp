#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/MergedMeshResources.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "common/assert.h"
#include <array>
#include <fstream>
#include "graphics/shader/meshAssembly.h"
#include "graphics/shader/meshPointAbi.h"
#include "graphics/shader/recompiler/meshBarrierPlan.h"
#include <cstdio>
#include <stdexcept>
#include <source_location>
using namespace Libs::Graphics::ShaderRecompiler::Decoder;
void Require(bool value, std::source_location where=std::source_location::current()) {
 if (!value) { std::fprintf(stderr,"ES regression failed at line %u\n",where.line()); std::exit(1); }
}
namespace Common {
int DbgExitHandler(char const*,int line,std::string_view text) { std::fprintf(stderr,"IR fatal line %d: %.*s\n",line,int(text.size()),text.data()); std::abort(); }
int DbgExitHandler(char const*,int,fmt::text_style,std::string_view) { std::abort(); }
int DbgExitIfHandler(char const*,char const*,int) { std::abort(); }
int DbgNotImplementedHandler(char const*,char const*,int) { std::abort(); }
void DbgExit(int) { std::abort(); }
}
std::vector<uint32_t> ReadCapturedCode(const char* path) {
 std::ifstream input(path,std::ios::binary|std::ios::ate);
 Require(input.is_open());
 const std::streamoff bytes=input.tellg();
 Require(bytes>0 && bytes<=128*1024 && bytes%4==0);
 std::vector<uint32_t> words(static_cast<size_t>(bytes)/4);
 input.seekg(0);
 Require(bool(input.read(reinterpret_cast<char*>(words.data()),bytes)));
 return words;
}
int main(int argc,char** argv) {
 Require(argc==1 || argc==3);
 if(argc==3) {
  const auto es=ReadCapturedCode(argv[1]);
  const auto gs=ReadCapturedCode(argv[2]);
  std::vector<uint32_t> joined;
  Program captured;
  std::string diagnostic;
  Require(DecodeFusedProgram(es,gs,&joined,&captured,&diagnostic));
  Libs::Graphics::ShaderRecompiler::CFG::Graph captured_graph;
  Require(Libs::Graphics::ShaderRecompiler::CFG::BuildGraph(captured,&captured_graph,&diagnostic));
  const bool structured=Libs::Graphics::ShaderRecompiler::CFG::StructurizeForLowering(&captured_graph,&diagnostic);
  Libs::Graphics::ShaderRecompiler::IR::Program captured_ir;
  Require(Libs::Graphics::ShaderRecompiler::IR::LowerProgram(captured,captured_graph,
      Libs::Graphics::ShaderType::Mesh,64,&captured_ir,&diagnostic));
  captured_ir.dispatcher_fallback=!structured;
  for(const auto& block:captured_ir.blocks) for(const auto& inst:block.instructions) {
   using namespace Libs::Graphics::ShaderRecompiler::IR;
   Require(inst.execution!=InstructionExecution::Unknown);
   if(inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::ReadLaneU32 ||
      inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::ReadFirstLaneU32)
    Require(inst.execution==InstructionExecution::Scalar);
  }
  // Run the real captured shader through the same resource planner as live GE capture.
  auto resource_ir=captured_ir;
  if(!Libs::Graphics::ShaderRecompiler::IR::BuildMergedMeshResourcePlan(&resource_ir,16,&diagnostic)) {
   std::fprintf(stderr,"Captured Mesh resource plan failed: %s\n",diagnostic.c_str());Require(false);
  }
  size_t vector_buffers=0,scalar_buffers=0,root_reads=0;
  for(const auto& block:resource_ir.blocks) for(const auto& inst:block.instructions) {
   using namespace Libs::Graphics::ShaderRecompiler::IR;
   if(inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::BufferLoadDword) ++vector_buffers;
   if(inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::SBufferLoadDword) ++scalar_buffers;
   if(inst.pc==0x68 && (inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::SLoadDword ||
                       inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::LoadSrtDword)) {
    Require(inst.scalar_value<resource_ir.provenance.values.size());
    const auto& value=resource_ir.provenance.values[inst.scalar_value];Require(value.op==ScalarValueOp::ReadConst);
    for(uint32_t word=0;word<2;++word) {
     Require(value.args[word]<resource_ir.provenance.values.size());
     const auto& root=resource_ir.provenance.values[value.args[word]];
     Require(root.op==ScalarValueOp::MeshGsUserData && root.imm==word);
    }
    ++root_reads;
   }
  }
  std::printf("[MESH-RESOURCES] GS-root=separate ES-base=8 vector-buffer-components=%zu scalar-buffer-components=%zu root-components=%zu buffers=%zu plan=passed GPU=not-executed\n",
              vector_buffers,scalar_buffers,root_reads,resource_ir.info.buffers.size());
  Require(vector_buffers==14 && root_reads==16);
  Libs::Graphics::ShaderRecompiler::MeshBarrierPlan phases;
  Require(Libs::Graphics::ShaderRecompiler::PlanMeshBarrierPhases(captured_ir,&phases));
  Require(phases.barrier_pcs==std::vector<uint32_t>({0xe0,0x864,0x8f8,0x970,0xa48}));
  Require(phases.segments.size()==69);
  size_t non_barrier=0, segmented=0, boundaries=0;
  for(const auto& block:captured_ir.blocks) for(const auto& inst:block.instructions)
   if(inst.op!=Libs::Graphics::ShaderRecompiler::IR::Opcode::Barrier) ++non_barrier;
  for(const auto& segment:phases.segments) {
   segmented+=segment.instruction_end-segment.instruction_begin;
   if(segment.ends_at_barrier) {
    ++boundaries;Require(segment.continuation<phases.segments.size());
    const auto& next=phases.segments[segment.continuation];
    Require(next.block_id==segment.block_id && next.phase==segment.phase+1 &&
            next.instruction_begin==segment.instruction_end+1);
   }
  }
  Require(non_barrier==segmented && boundaries==5);
  std::printf("[MESH-BARRIERS] phases=%zu segments=%zu static-order=passed convergence=not-executed\n",phases.barrier_pcs.size(),phases.segments.size());
  std::printf("[MESH-REPLAY] instructions=%zu blocks=%zu dispatcher=%u IR=passed GPU=not-executed\n",
      captured.instructions.size(),captured_ir.blocks.size(),unsigned(!structured));
 }

 // Materialize a GS-table-derived buffer through the same production plan as live capture.
 {
  namespace R=Libs::Graphics::ShaderRecompiler::IR;
  R::Program mesh;mesh.stage=Libs::Graphics::ShaderType::Mesh;mesh.wave_size=64;mesh.blocks.resize(1);
  for(uint32_t c=0;c<4;++c) {
   R::Instruction load;load.op=R::Opcode::SLoadDword;load.pc=0x68;
   load.execution=R::InstructionExecution::Scalar;load.dst.kind=R::OperandKind::Register;
   load.dst.reg={R::RegisterFile::Scalar,8+c};load.src_count=1;
   load.src[0].kind=R::OperandKind::ImmediateU32;load.memory.kind=R::ResourceKind::ScalarBuffer;
   load.memory.resource=0;load.memory.offset=4*c;load.memory.data_bits=32;load.memory.data_dwords=1;
   load.memory.component_count=4;load.memory.component_index=c;mesh.blocks[0].instructions.push_back(load);
  }
  R::Instruction buffer;buffer.op=R::Opcode::BufferLoadDword;buffer.pc=0xf8;
  buffer.execution=R::InstructionExecution::Vector;buffer.dst.kind=R::OperandKind::Register;
  buffer.dst.reg={R::RegisterFile::Vector,0};buffer.src_count=2;
  buffer.src[0].kind=R::OperandKind::Register;buffer.src[0].reg={R::RegisterFile::Vector,9};
  buffer.src[1].kind=R::OperandKind::ImmediateU32;
  buffer.memory.kind=R::ResourceKind::Buffer;buffer.memory.resource=2;buffer.memory.idxen=true;
  buffer.memory.data_bits=32;buffer.memory.data_dwords=1;buffer.memory.component_count=1;
  mesh.blocks[0].instructions.push_back(buffer);
  std::string error;Require(R::BuildMergedMeshResourcePlan(&mesh,16,&error));
  Require(mesh.info.buffers.size()==1 && mesh.info.addresses.empty());
  std::array<uint32_t,16> es_words{};es_words[0]=0x87654321;
  R::SrtRuntime runtime;runtime.user_data=es_words;runtime.mesh_gs_user_data_address=0x123450000ull;
  runtime.read_memory=[](void*,uint64_t address,uint32_t* word) {
   const uint64_t root=address&~uint64_t(0xffff);
   if((root!=0x123450000ull && root!=0x123460000ull) || address-root>=16 || address%4) return false;
   const uint32_t header[]={root==0x123450000ull?0x40000000u:0x42000000u,16u<<16,256,0};
   *word=header[(address-root)/4];return true;
  };
  R::ResourceSnapshot snapshot;Require(R::MaterializeResources(mesh,runtime,&snapshot,&error));
  Require(snapshot.buffers.size()==1 && snapshot.buffers[0].dwords[0]==0x40000000u);
  Require(snapshot.user_data==std::vector<uint32_t>(es_words.begin(),es_words.end()));
  runtime.mesh_gs_user_data_address=0x123460000ull;
  Require(R::MaterializeResources(mesh,runtime,&snapshot,&error));Require(snapshot.buffers[0].dwords[0]==0x42000000u);
  const auto saved=snapshot;runtime.mesh_gs_user_data_address.reset();
  Require(!R::MaterializeResources(mesh,runtime,&snapshot,&error));
  Require(snapshot.buffers==saved.buffers && snapshot.flattened_srt==saved.flattened_srt && snapshot.user_data==saved.user_data);
  std::puts("[MESH-RESOURCES] production GS-root buffer materialization refresh missing-root rollback=passed");
 }

 // Static phase analysis must reject bypasses, repeated barriers and early exit.
 {
  using namespace Libs::Graphics::ShaderRecompiler;
  IR::Program phases;phases.stage=Libs::Graphics::ShaderType::Mesh;phases.blocks.resize(3);
  for(uint32_t i=0;i<3;++i) phases.blocks[i].id=i;
  phases.blocks[0].terminator.kind=CFG::TerminatorKind::ConditionalBranch;
  phases.blocks[0].terminator.true_block=1;phases.blocks[0].terminator.false_block=2;
  phases.blocks[1].terminator.kind=CFG::TerminatorKind::Branch;phases.blocks[1].terminator.true_block=2;
  IR::Instruction barrier;barrier.op=IR::Opcode::Barrier;barrier.execution=IR::InstructionExecution::Scalar;barrier.pc=16;
  phases.blocks[2].instructions={barrier};
  barrier.pc=20;phases.blocks[2].instructions.push_back(barrier);
  MeshBarrierPlan plan;
  Require(PlanMeshBarrierPhases(phases,&plan));Require(plan.barrier_pcs==std::vector<uint32_t>({16,20}));
  Require(plan.segments.size()==5 && plan.block_first_segments[2]==2);
  Require(plan.segments[2].ends_at_barrier && plan.segments[2].continuation==3);
  Require(plan.segments[3].ends_at_barrier && plan.segments[3].continuation==4);
  Require(!plan.segments[4].ends_at_barrier && plan.segments[4].phase==2 &&
          plan.segments[4].instruction_begin==plan.segments[4].instruction_end);
  const auto saved=plan.barrier_pcs;
  phases.blocks[1].terminator.kind=CFG::TerminatorKind::Return;
  Require(!PlanMeshBarrierPhases(phases,&plan));Require(plan.barrier_pcs==saved);
  phases.blocks[1].terminator.kind=CFG::TerminatorKind::Branch;
  phases.blocks[2].terminator.kind=CFG::TerminatorKind::Branch;phases.blocks[2].terminator.true_block=0;
  Require(!PlanMeshBarrierPhases(phases,&plan));
  phases.blocks[2].terminator.kind=CFG::TerminatorKind::Return;
  phases.blocks[0].terminator.kind=CFG::TerminatorKind::IndirectBranch;
  Require(!PlanMeshBarrierPhases(phases,&plan));
  phases.blocks[0].terminator.kind=CFG::TerminatorKind::ConditionalBranch;
  phases.blocks[1].instructions.push_back(barrier);phases.blocks[2].instructions.pop_back();
  Require(!PlanMeshBarrierPhases(phases,&plan)); // one branch executes barrier20 before barrier16
 }
 Libs::Graphics::MeshAssemblyPlan assembly;
 Require(Libs::Graphics::PlanPointMeshAssembly(3,24,72,216,64,&assembly));
 Require(assembly.max_primitives==210 && assembly.primitives_per_group==3 &&
         assembly.vertices_per_group==3 && assembly.logical_invocations==256 && assembly.guest_waves==4);
 Require(Libs::Graphics::PlanPointMeshAssembly(3,24,72,216,32,&assembly));
 Require(assembly.logical_invocations==224 && assembly.guest_waves==7);
 const auto valid_assembly=assembly;
 Require(!Libs::Graphics::PlanPointMeshAssembly(3,24,72,71,64,&assembly));
 Require(assembly.logical_invocations==valid_assembly.logical_invocations);
 Require(!Libs::Graphics::PlanPointMeshAssembly(64,64,1024,1024,64,&assembly));
 Require(!Libs::Graphics::PlanPointMeshAssembly(3,24,72,1024,32,&assembly));
 Require(!Libs::Graphics::PlanPointMeshAssembly(3,24,72,216,16,&assembly));
 // Merged point ABI: partial groups, empty waves and both physical halves.
 {
  using namespace Libs::Graphics;
  MeshPointInitialRegisters regs;
  MeshPointDraw draw {7,19,23,2,4,0x12345678abcdef01ull};
  Require(PlanPointMeshAssembly(3,24,72,216,64,&assembly));
  Require(PlanMeshPointInitialRegisters(assembly,64,draw,0,&regs));
  Require(regs.s0==0xabcdef01u && regs.s1==0x12345678u && regs.s3==0x40000101u);
  Require(regs.v0==0 && regs.v1==0 && regs.v5==25 && regs.v8==27);
  Require(regs.es_user_data_base==8);
  const auto saved=regs;
  Require(!PlanMeshPointInitialRegisters(assembly,64,draw,256,&regs));
  Require(regs.s3==saved.s3 && regs.v5==saved.v5); // failure is transactional
  auto invalid=assembly;invalid.guest_waves=3;
  Require(!PlanMeshPointInitialRegisters(invalid,64,draw,0,&regs));
  Require(!PlanMeshPointInitialRegisters(assembly,16,draw,0,&regs));
  Require(!PlanMeshPointInitialRegisters(assembly,64,draw,0,nullptr));
  draw.group_x=0xffffffffu;
  Require(!PlanMeshPointInitialRegisters(assembly,64,draw,0,&regs));
  // Independently count valid input points per wave; do not derive expectations from S3.
  for(const uint32_t wave_size: {32u,64u}) {
   Require(PlanPointMeshAssembly(3,24,72,216,wave_size,&assembly));
   for(uint32_t total=0;total<=10;++total) for(uint32_t group=0;group<5;++group) {
    draw={total,19,23,group,4,0x12345678abcdef01ull};
    for(uint32_t lane=0;lane<assembly.logical_invocations;++lane) {
     Require(PlanMeshPointInitialRegisters(assembly,wave_size,draw,lane,&regs));
     uint32_t expected_count=0;
     const uint32_t wave=lane/wave_size;
     for(uint32_t point=0;point<3;++point)
      if(group*3+point<total && point/wave_size==wave) ++expected_count;
     Require((regs.s3&0xffu)==expected_count && ((regs.s3>>8)&0xffu)==expected_count);
     Require((regs.s3>>28)==assembly.guest_waves && ((regs.s3>>24)&15u)==wave);
     Require(regs.v0==lane*4 && regs.v1==0 && regs.v5==19+group*3+lane && regs.v8==27);
    }
   }
   // A 64-point input group exercises the wave32 boundary; wave64 has one populated wave.
   Require(PlanPointMeshAssembly(64,64,3,216,wave_size,&assembly));
   draw={63,0xffffffffu,0xffffffffu,0,1,0};
   for(uint32_t lane=0;lane<assembly.logical_invocations;++lane) {
    Require(PlanMeshPointInitialRegisters(assembly,wave_size,draw,lane,&regs));
    uint32_t count=0;
    for(uint32_t point=0;point<63;++point) if(point/wave_size==lane/wave_size) ++count;
    Require((regs.s3&0xffu)==count && ((regs.s3>>8)&0xffu)==count);
    Require(regs.v5==uint32_t(0xffffffffu+lane) && regs.v8==0); // guest u32 addition wraps
   }
  }
  std::puts("[MESH-POINT-ABI] partial-groups wave32/wave64 register-oracle=passed GPU=not-executed");
 }
 // Captured ES executable prefix, followed by inline non-instruction data.
 std::array<uint32_t,18> code {0x8f6a9003u,0x94fe6ac1u,0xbf88000bu,0xd7650005u,
  0x000100c1u,0x93eaff03u,0x00040018u,0xd7660005u,0x00020ac1u,0xd7460005u,
  0x4150c6au,0x340a0a82u,0xd8340000u,0x00000805u,0xbf8cc07fu,0xbe802006u,
  0xffffffffu,0xffffffffu};
 Program front; std::string error;
 Require(DecodeFrontProgram(code,&front,&error));
 Require(front.code.size()==16 && front.instructions.size()==11);
 Require(front.instructions.back().opcode==Opcode::SSetpcB64);
 Require(front.instructions.back().pc==60 && front.instructions.back().src0.reg==6);
 Program ordinary;
 Require(!DecodeProgram(code,&ordinary,&error)); // Normal stages still reject the data tail.
 auto bad=code; bad[15]=0xbe802004u; Require(!DecodeFrontProgram(bad,&front,&error));
 bad=code; bad[15]=0xbf810000u; Require(!DecodeFrontProgram(bad,&front,&error));
 Require(!DecodeFrontProgram(std::span(code).first(15),&front,&error));
 bad=code; bad[2]=0xbf88000du; Require(!DecodeFrontProgram(bad,&front,&error)); // Branch into data.
 bad=code; bad[2]=0xbf880000u; Require(DecodeFrontProgram(bad,&front,&error)); // Valid next instruction.
 bad=code; bad[2]=0xbf880001u; Require(!DecodeFrontProgram(bad,&front,&error)); // Middle of a 2-DWORD op.
 // Fused ES/GS keeps one register/LDS lifetime and relocates GS branches.
 std::vector<uint32_t> joined; Program fused;
 std::array<uint32_t,3> back {0xbf820001u,0xbf800000u,0xbf810000u};
 Require(DecodeFusedProgram(code,back,&joined,&fused,&error));
 Require(joined.size()==19 && fused.code.data()==joined.data());
 Require(fused.instructions[10].opcode==Opcode::SBranch && fused.instructions[10].branch_target==64);
 Require(fused.instructions[11].pc==64 && fused.instructions[11].branch_target==72);
 Require(fused.instructions.back().pc==72);
 Libs::Graphics::ShaderRecompiler::CFG::Graph graph;
 Require(Libs::Graphics::ShaderRecompiler::CFG::BuildGraph(fused,&graph,&error));
 Require(graph.FindBlockByPc(64)!=nullptr);
 Require(Libs::Graphics::ShaderRecompiler::CFG::Structurize(&graph,&error));
 Libs::Graphics::ShaderRecompiler::IR::Program ir;
 Require(Libs::Graphics::ShaderRecompiler::IR::LowerProgram(fused,graph,
     Libs::Graphics::ShaderType::Mesh,64,&ir,&error));
 Require(ir.stage==Libs::Graphics::ShaderType::Mesh && !ir.blocks.empty());
 bool retains_es_shift_add=false,retains_es_shift=false;
 bool retains_ds_write=false;
 for(const auto& block:ir.blocks) for(const auto& inst:block.instructions) {
  Require(inst.execution!=Libs::Graphics::ShaderRecompiler::IR::InstructionExecution::Unknown);
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  if(inst.pc==0x24 && inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::ShiftLeftAddU32) {
   retains_es_shift_add=true;Require(inst.execution==InstructionExecution::Vector);
   Require(inst.dst.reg.file==RegisterFile::Vector && inst.dst.reg.index==5 && inst.src_count==3);
   Require(inst.src[1].kind==Libs::Graphics::ShaderRecompiler::IR::OperandKind::ImmediateU32 && inst.src[1].imm==6);
   Require(inst.src[2].reg.file==RegisterFile::Vector && inst.src[2].reg.index==5);
  }
  if(inst.pc==0x2c && inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::ShiftLeftLogicalU32) {
   retains_es_shift=true;Require(inst.execution==InstructionExecution::Vector);
   Require(inst.dst.reg.index==5 && inst.src_count==2 && inst.src[0].reg.index==5 && inst.src[1].imm==2);
  }
  if(inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::DsWriteB32) {
   retains_ds_write=true;
   Require(inst.execution==Libs::Graphics::ShaderRecompiler::IR::InstructionExecution::Vector);
  }
 }
 Require(retains_es_shift_add && retains_es_shift);
 Require(retains_ds_write); // ES-to-GS shared-memory operation survives fused lowering.

 back[0]=0xbf82ffffu; Require(DecodeFusedProgram(code,back,&joined,&fused,&error)); // GS-local loop.
 Require(fused.instructions[11].branch_target==64);
 back[0]=0xbf82fffeu; Require(!DecodeFusedProgram(code,back,&joined,&fused,&error)); // Cannot jump into ES.
 back[0]=0xbe801f00u; Require(!DecodeFusedProgram(code,back,&joined,&fused,&error)); // Needs GS guest base.
 std::array<uint32_t,4> middle {0xbf820001u,0xd7650005u,0x000100c1u,0xbf810000u};
 Require(!DecodeFusedProgram(code,middle,&joined,&fused,&error));
 Require(!DecodeFusedProgram(code,{},&joined,&fused,&error));
 // Captured GS SENDMSG9 allocation must read the preceding M0 write.
 std::array<uint32_t,4> allocation {0xbefc03ffu,0x00001003u,0xbf900009u,0xbf810000u};
 Require(DecodeFusedProgram(code,allocation,&joined,&fused,&error));
 Require(Libs::Graphics::ShaderRecompiler::CFG::BuildGraph(fused,&graph,&error));
 Libs::Graphics::ShaderRecompiler::CFG::StructurizeForLowering(&graph,&error);
 for(auto stage:{Libs::Graphics::ShaderType::Mesh,Libs::Graphics::ShaderType::Vertex}) {
  Require(Libs::Graphics::ShaderRecompiler::IR::LowerProgram(fused,graph,stage,64,&ir,&error));
  bool found=false;
  for(const auto& b:ir.blocks) for(const auto& instruction:b.instructions) {
   if(instruction.op!=Libs::Graphics::ShaderRecompiler::IR::Opcode::Sendmsg) continue;
   found=true;
   Require(instruction.execution==Libs::Graphics::ShaderRecompiler::IR::InstructionExecution::Scalar);
   Require(instruction.src[0].imm==9);
   if(stage==Libs::Graphics::ShaderType::Mesh) {
    Require(instruction.src_count==2 && instruction.src[1].kind==Libs::Graphics::ShaderRecompiler::IR::OperandKind::Register);
    Require(instruction.src[1].reg.file==Libs::Graphics::ShaderRecompiler::IR::RegisterFile::M0);
   } else Require(instruction.src_count==1);
  }
  Require(found);
 }
 // Captured Astro control-flow shape; non-control instructions replaced with NOPs.
 std::vector<uint32_t> nested(750,0xbf800000u);
 nested[2]=0xbf88000bu;
 nested[15]=0xbf820000u;
 nested[39]=0xbf880004u;
 nested[49]=0xbf880004u;
 nested[60]=0xbf8801dau;
 nested[66]=0xbf8801d4u;
 nested[74]=0xbf880006u;
 nested[165]=0xbf88000du;
 nested[210]=0xbf88000du;
 nested[257]=0xbf880019u;
 nested[271]=0xbf88000bu;
 nested[286]=0xbf88000du;
 nested[303]=0xbf88000du;
 nested[350]=0xbf880019u;
 nested[364]=0xbf88000bu;
 nested[379]=0xbf88000du;
 nested[396]=0xbf88000du;
 nested[443]=0xbf880019u;
 nested[457]=0xbf88000bu;
 nested[472]=0xbf88000du;
 nested[488]=0xbf88000du;
 nested[505]=0xbf880019u;
 nested[519]=0xbf88000bu;
 nested[534]=0xbf85fe6du;
 nested[549]=0xbf88000du;
 nested[564]=0xbf880007u;
 nested[609]=0xbf880003u;
 nested[632]=0xbf880007u;
 nested[645]=0xbf880004u;
 nested[651]=0xbf880003u;
 nested[662]=0xbf880016u;
 nested[710]=0xbf850004u;
 nested[722]=0xbf88000bu;
 nested[737]=0xbf88000bu;
 nested[742]=0xbf840006u;
 nested[749]=0xbf810000u;
 Program selection; Require(DecodeProgram(nested,&selection,&error));
 Require(Libs::Graphics::ShaderRecompiler::CFG::BuildGraph(selection,&graph,&error));
 const auto original=graph;
 Require(!Libs::Graphics::ShaderRecompiler::CFG::StructurizeForLowering(&graph,&error));
 Require(graph.unsupported && graph.failure_kind==Libs::Graphics::ShaderRecompiler::CFG::FailureKind::StructuredControlFlow);
 Require(graph.blocks.size()==original.blocks.size());
 for(size_t i=0;i<graph.blocks.size();++i) {
  Require(graph.blocks[i].successors==original.blocks[i].successors);
  Require(graph.blocks[i].terminator.merge_block==original.blocks[i].terminator.merge_block);
 }
 Require(Libs::Graphics::ShaderRecompiler::IR::LowerProgram(selection,graph,
     Libs::Graphics::ShaderType::Mesh,64,&ir,&error));
 std::puts("Captured ES handoff, inline data, branch boundaries and ordinary decoder passed");
}
