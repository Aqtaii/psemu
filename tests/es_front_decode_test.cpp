#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "common/assert.h"
#include <array>
#include <fstream>
#include "graphics/shader/meshAssembly.h"
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
 bool retains_ds_write=false;
 for(const auto& block:ir.blocks) for(const auto& inst:block.instructions) {
  Require(inst.execution!=Libs::Graphics::ShaderRecompiler::IR::InstructionExecution::Unknown);
  if(inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::DsWriteB32) {
   retains_ds_write=true;
   Require(inst.execution==Libs::Graphics::ShaderRecompiler::IR::InstructionExecution::Vector);
  }
 }
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
