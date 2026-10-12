#pragma once
#include "graphics/shader/recompiler/ShaderIR.h"
#include <algorithm>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {
struct MeshBarrierPlan {
 std::vector<uint32_t> barrier_pcs;
 std::vector<uint32_t> block_entry_phases;
};
// Restricted static phase plan: every reachable path must encounter the same
// barriers in increasing PC order. Barrier-free loops are allowed; loops that
// repeat a barrier, bypasses, early returns and indirect control flow are rejected.
// This proves ordering only, not termination or native barrier convergence.
inline bool PlanMeshBarrierPhases(const IR::Program& program, MeshBarrierPlan* output) {
 if(output==nullptr || program.stage!=ShaderType::Mesh || program.blocks.empty()) return false;
 MeshBarrierPlan plan;
 std::vector<bool> reachable(program.blocks.size(),false);
 std::vector<uint32_t> work {0};
 auto successors=[&](const IR::BasicBlock& block,std::vector<uint32_t>& next) {
  switch(block.terminator.kind) {
   case CFG::TerminatorKind::Branch: next.push_back(block.terminator.true_block);break;
   case CFG::TerminatorKind::ConditionalBranch:
    next.push_back(block.terminator.true_block);next.push_back(block.terminator.false_block);break;
   case CFG::TerminatorKind::Return:break;
   default:return false;
  }
  return true;
 };
 while(!work.empty()) {
  const auto id=work.back();work.pop_back();
  if(id>=program.blocks.size() || program.blocks[id].id!=id) return false;
  if(reachable[id]) continue;
  reachable[id]=true;
  const auto& block=program.blocks[id];
  for(const auto& inst:block.instructions) if(inst.op==IR::Opcode::Barrier) {
   if(inst.execution!=IR::InstructionExecution::Scalar) return false;
   plan.barrier_pcs.push_back(inst.pc);
  }
  if(!successors(block,work)) return false;
 }
 std::sort(plan.barrier_pcs.begin(),plan.barrier_pcs.end());
 if(std::adjacent_find(plan.barrier_pcs.begin(),plan.barrier_pcs.end())!=plan.barrier_pcs.end()) return false;
 plan.block_entry_phases.assign(program.blocks.size(),UINT32_MAX);
 std::vector<std::pair<uint32_t,uint32_t>> pending {{0,0}};
 bool has_return=false;
 while(!pending.empty()) {
  const auto [id,phase]=pending.back();pending.pop_back();
  auto& entry=plan.block_entry_phases[id];
  if(entry!=UINT32_MAX) {if(entry!=phase) return false;continue;}
  entry=phase;
  auto after=phase;
  const auto& block=program.blocks[id];
  for(const auto& inst:block.instructions) if(inst.op==IR::Opcode::Barrier) {
   if(after>=plan.barrier_pcs.size() || inst.pc!=plan.barrier_pcs[after]) return false;
   ++after;
  }
  if(block.terminator.kind==CFG::TerminatorKind::Return) {
   if(after!=plan.barrier_pcs.size()) return false;
   has_return=true;
  }
  std::vector<uint32_t> next;
  if(!successors(block,next)) return false;
  for(auto target:next) pending.emplace_back(target,after);
 }
 if(!has_return) return false;
 *output=std::move(plan);
 return true;
}
} // namespace Libs::Graphics::ShaderRecompiler
