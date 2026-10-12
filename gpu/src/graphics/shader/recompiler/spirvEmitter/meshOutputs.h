#pragma once
#include "graphics/shader/recompiler/SpirvBuilder.h"
#include <spirv/unified1/spirv.hpp>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv {
struct MeshOutputLayout {
 uint32_t position=0, triangles=0, cull=0;
 uint32_t pointer_position=0, pointer_triangles=0, pointer_cull=0;
 uint32_t u32=0, boolean=0, triangle_type=0;
};
// Counts are already validated by assembly/device limits. Constants are IDs.
template<class Constant>
MeshOutputLayout DefineMeshOutputs(Builder& b,uint32_t u32,uint32_t boolean,
                                  uint32_t vec4,uint32_t vec3uint,
                                  uint32_t vertices,uint32_t primitives,Constant constant) {
 MeshOutputLayout out;
 out.u32=u32; out.boolean=boolean; out.triangle_type=vec3uint;
 auto array=[&](uint32_t element,uint32_t count,uint32_t& pointer) {
  const auto array_type=b.AllocateId(), array_pointer=b.AllocateId();
  b.AddType({spv::OpTypeArray,array_type,element,constant(count)});
  b.AddType({spv::OpTypePointer,array_pointer,spv::StorageClassOutput,array_type});
  pointer=b.AllocateId();
  b.AddType({spv::OpTypePointer,pointer,spv::StorageClassOutput,element});
  const auto variable=b.AllocateId();
  b.AddType({spv::OpVariable,array_pointer,variable,spv::StorageClassOutput});
  return variable;
 };
 out.position=array(vec4,vertices,out.pointer_position);
 out.triangles=array(vec3uint,primitives,out.pointer_triangles);
 out.cull=array(boolean,primitives,out.pointer_cull);
 b.AddAnnotation({spv::OpDecorate,out.position,spv::DecorationBuiltIn,spv::BuiltInPosition});
 b.AddAnnotation({spv::OpDecorate,out.triangles,spv::DecorationBuiltIn,spv::BuiltInPrimitiveTriangleIndicesEXT});
 b.AddAnnotation({spv::OpDecorate,out.cull,spv::DecorationBuiltIn,spv::BuiltInCullPrimitiveEXT});
 b.AddAnnotation({spv::OpDecorate,out.cull,spv::DecorationPerPrimitiveEXT});
 return out;
}
struct MeshParameterOutput {
 uint32_t variable=0, pointer=0;
};
// Vertex attributes use the same locations as the linked fragment inputs.
template<class Constant>
MeshParameterOutput DefineMeshParameterOutput(Builder& b,uint32_t vec4,
                                              uint32_t vertices,uint32_t location,Constant constant) {
 const auto array_type=b.AllocateId(), array_pointer=b.AllocateId();
 b.AddType({spv::OpTypeArray,array_type,vec4,constant(vertices)});
 b.AddType({spv::OpTypePointer,array_pointer,spv::StorageClassOutput,array_type});
 MeshParameterOutput out{b.AllocateId(),b.AllocateId()};
 b.AddType({spv::OpTypePointer,out.pointer,spv::StorageClassOutput,vec4});
 b.AddType({spv::OpVariable,array_pointer,out.variable,spv::StorageClassOutput});
 b.AddAnnotation({spv::OpDecorate,out.variable,spv::DecorationLocation,location});
 return out;
}
inline void StoreMeshParameter(Builder& b,const MeshParameterOutput& output,
                               uint32_t index,uint32_t value) {
 const auto pointer=b.AllocateId();
 b.AddFunction({spv::OpAccessChain,output.pointer,pointer,output.variable,index});
 b.AddFunction({spv::OpStore,pointer,value});
}
// Uniform entry-point allocation after all guest waves have completed.
template<class Constant>
void SetMeshOutputsFromPacked(Builder& b,const MeshOutputLayout& layout,uint32_t allocation,Constant constant) {
 const auto vertices=b.AllocateId(), primitives=b.AllocateId();
 b.AddFunction({spv::OpBitFieldUExtract,layout.u32,vertices,allocation,constant(0),constant(10)});
 b.AddFunction({spv::OpBitFieldUExtract,layout.u32,primitives,allocation,constant(12),constant(11)});
 b.AddFunction({spv::OpSetMeshOutputsEXT,vertices,primitives});
}
inline void StoreMeshPosition(Builder& b,const MeshOutputLayout& layout,uint32_t index,uint32_t value) {
 const auto pointer=b.AllocateId();
 b.AddFunction({spv::OpAccessChain,layout.pointer_position,pointer,layout.position,index});
 b.AddFunction({spv::OpStore,pointer,value});
}
// Caller has uniformly executed SetMeshOutputs and checked index < primitive count.
template<class Constant>
void StoreMeshPackedTriangle(Builder& b,const MeshOutputLayout& layout,uint32_t index,
                             uint32_t packed,Constant constant) {
 std::array<uint32_t,3> vertices;
 for(uint32_t i=0;i<3;++i) {
  vertices[i]=b.AllocateId();
  b.AddFunction({spv::OpBitFieldUExtract,layout.u32,vertices[i],packed,constant(i*10),constant(10)});
 }
 const auto triangle=b.AllocateId(), pointer=b.AllocateId();
 b.AddFunction({spv::OpCompositeConstruct,layout.triangle_type,triangle,vertices[0],vertices[1],vertices[2]});
 b.AddFunction({spv::OpAccessChain,layout.pointer_triangles,pointer,layout.triangles,index});
 b.AddFunction({spv::OpStore,pointer,triangle});
 const auto flag=b.AllocateId(), culled=b.AllocateId(), cull_pointer=b.AllocateId();
 b.AddFunction({spv::OpBitwiseAnd,layout.u32,flag,packed,constant(0x80000000u)});
 b.AddFunction({spv::OpINotEqual,layout.boolean,culled,flag,constant(0)});
 b.AddFunction({spv::OpAccessChain,layout.pointer_cull,cull_pointer,layout.cull,index});
 b.AddFunction({spv::OpStore,cull_pointer,culled});
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv
