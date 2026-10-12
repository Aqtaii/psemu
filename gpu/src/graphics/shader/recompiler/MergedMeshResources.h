#pragma once
#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "graphics/shader/recompiler/SrtWalker.h"
#include "graphics/shader/recompiler/SrtPatcher.h"
#include "graphics/shader/recompiler/ResourceTracking.h"
#include <utility>
namespace Libs::Graphics::ShaderRecompiler::IR {
// Shared by the live fused ES/GS capture and offline replay. Never infer S3 as user data.
// Build on a copy so unsupported resource contracts leave the caller's IR unchanged.
inline bool BuildMergedMeshResourcePlan(Program* program, uint32_t es_user_count, std::string* error) {
 if (!program || program->stage != ShaderType::Mesh) {
  if (error) *error="merged resource planning requires a Mesh program";
  return false;
 }
 auto next=*program;
 next.user_data_base=8;next.user_data_count=es_user_count;
 next.mesh_gs_user_data=true;next.uses_guest_address_table=true;
 if (!BuildScalarProvenance(&next,error) || !BuildSrtPlan(&next,error) ||
     !PatchSrtReads(&next,error) || !TrackResources(&next,error)) return false;
 *program=std::move(next);
 return true;
}
}
