#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {
uint32_t MeshIndex(EmitterState* state) {
 const auto id=state->builder.AllocateId();
 state->builder.AddFunction({spv::OpLoad,state->uint_type,id,state->mesh_local_index});
 return id;
}
template<class Body> void MeshIf(EmitterState* state,uint32_t condition,Body body) {
 const auto yes=state->builder.AllocateId(), end=state->builder.AllocateId();
 state->builder.AddFunction({spv::OpSelectionMerge,end,spv::SelectionControlMaskNone});
 state->builder.AddFunction({spv::OpBranchConditional,condition,yes,end});
 state->builder.AddFunction({spv::OpLabel,yes}); body();
 state->builder.AddFunction({spv::OpBranch,end}); state->builder.AddFunction({spv::OpLabel,end});
}
uint32_t MeshLoad(EmitterState* state,uint32_t type,uint32_t variable) {
 const auto id=state->builder.AllocateId();
 state->builder.AddFunction({spv::OpLoad,type,id,variable}); return id;
}
void MeshBarrier(EmitterState* state) {
 state->builder.AddFunction({spv::OpControlBarrier,ConstantU32(state,2),ConstantU32(state,2),ConstantU32(state,0x108)});
}
uint32_t MeshCount(EmitterState* state,uint32_t packed,uint32_t offset,uint32_t width,uint32_t capacity) {
 const auto raw=state->builder.AllocateId(), over=state->builder.AllocateId(), count=state->builder.AllocateId();
 state->builder.AddFunction({spv::OpBitFieldUExtract,state->uint_type,raw,packed,ConstantU32(state,offset),ConstantU32(state,width)});
 state->builder.AddFunction({spv::OpUGreaterThan,state->bool_type,over,raw,ConstantU32(state,capacity)});
 state->builder.AddFunction({spv::OpSelect,state->uint_type,count,over,ConstantU32(state,capacity),raw});
 return count;
}
}

uint32_t EmitMeshGuestLocalInvocationIndex(EmitterState* state) {
 const auto host=MeshIndex(state);
 if(state->lanes_per_invocation!=2) return host;
 // One native wave32 represents a guest wave64, matching the pinned Jetsku mapping.
 const auto wave_base=EmitBinaryU32(state,OpBitwiseAnd,host,ConstantU32(state,~31u));
 return EmitAddU32(state,EmitAddU32(state,host,wave_base),ConstantU32(state,state->lane_half*32u));
}

void DefineNativeMeshVariables(EmitterState* state) {
 auto& b=state->builder; const auto& mesh=state->vertex_input_info->mesh;
 const auto constant=[&](uint32_t value){return ConstantU32(state,value);};
 b.AddCapability({spv::CapabilityMeshShadingEXT}); b.AddExtension("SPV_EXT_mesh_shader");
 state->mesh_guest_func=b.AllocateId();
 state->mesh_outputs=DefineMeshOutputs(b,state->uint_type,state->bool_type,state->vec4_float_type,
                                      state->vec3_uint_type,mesh.max_vertices,mesh.max_primitives,constant);
 state->mesh_ptr_private_uint=b.AllocateId(); state->mesh_ptr_private_vec4=b.AllocateId();
 const auto ptr_shared=b.AllocateId();
 b.AddType({spv::OpTypePointer,state->mesh_ptr_private_uint,spv::StorageClassPrivate,state->uint_type});
 b.AddType({spv::OpTypePointer,state->mesh_ptr_private_vec4,spv::StorageClassPrivate,state->vec4_float_type});
 b.AddType({spv::OpTypePointer,ptr_shared,spv::StorageClassWorkgroup,state->uint_type});
 state->mesh_allocation=b.AllocateId();
 b.AddType({spv::OpVariable,ptr_shared,state->mesh_allocation,spv::StorageClassWorkgroup});
 for(uint32_t half=0;half<state->lanes_per_invocation;++half) {
  state->mesh_primitive_data[half]=b.AllocateId();
  b.AddType({spv::OpVariable,state->mesh_ptr_private_uint,state->mesh_primitive_data[half],spv::StorageClassPrivate});
 }
 for(auto& output:state->outputs) {
  if(output.kind==IR::StageOutputKind::Position) output.variable_id=state->mesh_outputs.position;
  else output.variable_id=DefineMeshParameterOutput(b,state->vec4_float_type,mesh.max_vertices,output.location,constant).variable;
  for(uint32_t half=0;half<state->lanes_per_invocation;++half) {
   output.mesh_data[half]=b.AllocateId();
   b.AddType({spv::OpVariable,state->mesh_ptr_private_vec4,output.mesh_data[half],spv::StorageClassPrivate});
  }
 }
 for(const auto& input:state->inputs) if(input.kind==IR::StageInputKind::LocalInvocationIndex) state->mesh_local_index=input.variable_id;
 if(state->mesh_local_index==0) {
  state->mesh_local_index=b.AllocateId();
  b.AddType({spv::OpVariable,state->ptr_input_uint,state->mesh_local_index,spv::StorageClassInput});
  b.AddAnnotation({spv::OpDecorate,state->mesh_local_index,spv::DecorationBuiltIn,spv::BuiltInLocalInvocationIndex});
 }
 b.AddExecutionMode({state->main_func,spv::ExecutionModeLocalSize,mesh.invocations,1,1});
 b.AddExecutionMode({state->main_func,spv::ExecutionModeOutputVertices,mesh.max_vertices});
 b.AddExecutionMode({state->main_func,spv::ExecutionModeOutputPrimitivesEXT,mesh.max_primitives});
 b.AddExecutionMode({state->main_func,spv::ExecutionModeOutputTrianglesEXT});
 b.AddEntryPoint(spv::ExecutionModelMeshEXT,state->main_func,"main",b.GlobalVariables());
}

void EmitNativeMeshAllocate(EmitterState* state,const IR::Instruction& inst) {
 const auto first=state->builder.AllocateId();
 state->builder.AddFunction({spv::OpIEqual,state->bool_type,first,MeshIndex(state),ConstantU32(state,0)});
 MeshIf(state,first,[&]{state->builder.AddFunction({spv::OpStore,state->mesh_allocation,EmitValueLoad(state,inst.src[1])});});
}

void EmitNativeMeshEntryPoint(EmitterState* state) {
 auto& b=state->builder; const auto& mesh=state->vertex_input_info->mesh;
 b.AddFunction({spv::OpFunction,state->void_type,state->main_func,spv::FunctionControlMaskNone,state->func_type});
 b.AddFunction({spv::OpLabel,b.AllocateId()});
 const auto zero4=b.AllocateId();
 b.AddType({spv::OpConstantNull,state->vec4_float_type,zero4});
 for(uint32_t half=0;half<state->lanes_per_invocation;++half) {
  for(const auto& output:state->outputs) b.AddFunction({spv::OpStore,output.mesh_data[half],zero4});
  b.AddFunction({spv::OpStore,state->mesh_primitive_data[half],ConstantU32(state,0x80000000u)});
 }
 const auto index=MeshIndex(state), first=b.AllocateId();
 b.AddFunction({spv::OpIEqual,state->bool_type,first,index,ConstantU32(state,0)});
 MeshIf(state,first,[&]{b.AddFunction({spv::OpStore,state->mesh_allocation,ConstantU32(state,0)});});
 MeshBarrier(state);
 b.AddFunction({spv::OpFunctionCall,state->void_type,b.AllocateId(),state->mesh_guest_func});
 // SetMeshOutputs must be outside divergent guest control flow.
 MeshBarrier(state);
 const auto allocation=MeshLoad(state,state->uint_type,state->mesh_allocation);
 const auto vertices=MeshCount(state,allocation,0,10,mesh.max_vertices);
 const auto primitives=MeshCount(state,allocation,12,11,mesh.max_primitives);
 b.AddFunction({spv::OpSetMeshOutputsEXT,vertices,primitives});
 const auto saved_half=state->lane_half;
 for(uint32_t half=0;half<state->lanes_per_invocation;++half) {
 state->lane_half=half;
 const auto index=EmitMeshGuestLocalInvocationIndex(state);
 const auto vertex=b.AllocateId();
 b.AddFunction({spv::OpULessThan,state->bool_type,vertex,index,vertices});
 MeshIf(state,vertex,[&]{
  for(const auto& output:state->outputs) {
   const auto value=MeshLoad(state,state->vec4_float_type,output.mesh_data[half]);
   if(output.kind==IR::StageOutputKind::Position) StoreMeshPosition(b,state->mesh_outputs,index,value);
   else StoreMeshParameter(b,{output.variable_id,state->ptr_output_vec4_float},index,value);
  }
 });
 const auto primitive=b.AllocateId();
 b.AddFunction({spv::OpULessThan,state->bool_type,primitive,index,primitives});
 MeshIf(state,primitive,[&]{StoreMeshPackedTriangle(b,state->mesh_outputs,index,
  MeshLoad(state,state->uint_type,state->mesh_primitive_data[half]),[&](uint32_t value){return ConstantU32(state,value);});});
 }
 state->lane_half=saved_half;
 b.AddFunction({spv::OpReturn}); b.AddFunction({spv::OpFunctionEnd});
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
