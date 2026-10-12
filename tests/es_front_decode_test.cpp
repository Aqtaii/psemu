#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "common/assert.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace Libs::Graphics::ShaderRecompiler::Decoder;
void Require(bool value) { if (!value) throw std::runtime_error("ES decoder regression"); }
namespace Common {
int DbgExitHandler(char const*,int,std::string_view) { std::abort(); }
int DbgExitHandler(char const*,int,fmt::text_style,std::string_view) { std::abort(); }
int DbgExitIfHandler(char const*,char const*,int) { std::abort(); }
int DbgNotImplementedHandler(char const*,char const*,int) { std::abort(); }
void DbgExit(int) { std::abort(); }
}
int main() {
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
  if(inst.op==Libs::Graphics::ShaderRecompiler::IR::Opcode::DsWriteB32) retains_ds_write=true;
 }
 Require(retains_ds_write); // ES-to-GS shared-memory operation survives fused lowering.

 back[0]=0xbf82ffffu; Require(DecodeFusedProgram(code,back,&joined,&fused,&error)); // GS-local loop.
 Require(fused.instructions[11].branch_target==64);
 back[0]=0xbf82fffeu; Require(!DecodeFusedProgram(code,back,&joined,&fused,&error)); // Cannot jump into ES.
 back[0]=0xbe801f00u; Require(!DecodeFusedProgram(code,back,&joined,&fused,&error)); // Needs GS guest base.
 std::array<uint32_t,4> middle {0xbf820001u,0xd7650005u,0x000100c1u,0xbf810000u};
 Require(!DecodeFusedProgram(code,middle,&joined,&fused,&error));
 Require(!DecodeFusedProgram(code,{},&joined,&fused,&error));
 // Nested selections share one merge; failed structuring must retain original CFG edges.
 std::array<uint32_t,4> nested {0xbf840002u,0xbf850001u,0xbf800000u,0xbf810000u};
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
