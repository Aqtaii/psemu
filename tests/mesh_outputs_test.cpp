#include "graphics/shader/recompiler/spirvEmitter/meshOutputs.h"
#include <cstdio>
#include <cassert>
#include "graphics/shader/recompiler/meshOutputProbe.h"
#include <map>
using namespace Libs::Graphics::ShaderRecompiler::Spirv;
int main() {
 using Libs::Graphics::ShaderRecompiler::MeshOutputProbeIncludesDraw;
 assert(MeshOutputProbeIncludesDraw(1,512,0,1));
 assert(MeshOutputProbeIncludesDraw(1,512,0,511));
 assert(!MeshOutputProbeIncludesDraw(1,512,0,512));
 assert(!MeshOutputProbeIncludesDraw(1,1,0,1));
 assert(MeshOutputProbeIncludesDraw(1,1,7,7));
 assert(MeshOutputProbeIncludesDraw(1,2,UINT32_MAX,0));
 assert(!MeshOutputProbeIncludesDraw(2,512,0,1));
 assert(!MeshOutputProbeIncludesDraw(1,0,0,0));
 Builder b(0x00010400u);
 const auto v=b.AllocateId(), boolean=b.AllocateId(), u=b.AllocateId(), f=b.AllocateId();
 const auto vec4=b.AllocateId(), vec3=b.AllocateId(), ft=b.AllocateId(), main=b.AllocateId();
 b.AddCapability({spv::CapabilityShader});
 b.AddCapability({spv::CapabilityMeshShadingEXT});
 b.AddExtension("SPV_EXT_mesh_shader");
 b.AddMemoryModel({spv::AddressingModelLogical,spv::MemoryModelGLSL450});
 b.AddType({spv::OpTypeVoid,v}); b.AddType({spv::OpTypeBool,boolean});
 b.AddType({spv::OpTypeInt,u,32,0}); b.AddType({spv::OpTypeFloat,f,32});
 b.AddType({spv::OpTypeVector,vec4,f,4}); b.AddType({spv::OpTypeVector,vec3,u,3});
 b.AddType({spv::OpTypeFunction,ft,v});
 std::map<uint32_t,uint32_t> constants;
 auto constant=[&](uint32_t value) {
  auto [it,fresh]=constants.try_emplace(value,0);
  if(fresh){it->second=b.AllocateId(); b.AddType({spv::OpConstant,u,it->second,value});}
  return it->second;
 };
 const auto out=DefineMeshOutputs(b,u,boolean,vec4,vec3,216,210,constant);
 const auto color=DefineMeshParameterOutput(b,vec4,216,0,constant);
 const auto modulation=DefineMeshParameterOutput(b,vec4,216,1,constant);
 b.AddEntryPoint(spv::ExecutionModelMeshEXT,main,"main",{out.position,out.triangles,out.cull,color.variable,modulation.variable});
 const auto fzero=b.AllocateId(), fone=b.AllocateId(), red_color=b.AllocateId(), white=b.AllocateId();
 b.AddType({spv::OpConstant,f,fzero,0}); b.AddType({spv::OpConstant,f,fone,0x3f800000u});
 b.AddType({spv::OpConstantComposite,vec4,red_color,fone,fzero,fzero,fone});
 b.AddType({spv::OpConstantComposite,vec4,white,fone,fone,fone,fone});
 b.AddExecutionMode({main,spv::ExecutionModeLocalSize,1,1,1});
 b.AddExecutionMode({main,spv::ExecutionModeOutputVertices,216});
 b.AddExecutionMode({main,spv::ExecutionModeOutputPrimitivesEXT,210});
 b.AddExecutionMode({main,spv::ExecutionModeOutputTrianglesEXT});
 b.AddFunction({spv::OpFunction,v,main,spv::FunctionControlMaskNone,ft});
 b.AddFunction({spv::OpLabel,b.AllocateId()});
 SetMeshOutputsFromPacked(b,out,constant(3u|(1u<<12)),constant);
 const uint32_t positions[3][4]={{0xbf800000u,0xbf800000u,0,0x3f800000u},
                              {0x40400000u,0xbf800000u,0,0x3f800000u},
                              {0xbf800000u,0x40400000u,0,0x3f800000u}};
 for(uint32_t vertex=0;vertex<3;++vertex) {
  uint32_t c[4];
  for(uint32_t i=0;i<4;++i){c[i]=b.AllocateId(); b.AddType({spv::OpConstant,f,c[i],positions[vertex][i]});}
  const auto value=b.AllocateId();
  b.AddType({spv::OpConstantComposite,vec4,value,c[0],c[1],c[2],c[3]});
  StoreMeshPosition(b,out,constant(vertex),value);
  StoreMeshParameter(b,color,constant(vertex),red_color);
  StoreMeshParameter(b,modulation,constant(vertex),white);
 }
 StoreMeshPackedTriangle(b,out,constant(0),constant((1u<<10)|(2u<<20)),constant);
 b.AddFunction({spv::OpReturn}); b.AddFunction({spv::OpFunctionEnd});
 const auto words=b.Build();
 if(words[1]!=0x00010400u || Builder{}.Build()[1]!=0x00010300u) return 1;
 auto* file=std::fopen("mesh_outputs.spv","wb"); if(!file) return 2;
 const bool ok=std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size();
 if(std::fclose(file)!=0 || !ok) return 3;
 Builder fragment;
 const auto fv=fragment.AllocateId(), ff=fragment.AllocateId(), fvec=fragment.AllocateId();
 const auto fp=fragment.AllocateId(), output=fragment.AllocateId(), fft=fragment.AllocateId();
 const auto fm=fragment.AllocateId(), zero=fragment.AllocateId(), one=fragment.AllocateId(), red=fragment.AllocateId();
 fragment.AddCapability({spv::CapabilityShader});
 fragment.AddMemoryModel({spv::AddressingModelLogical,spv::MemoryModelGLSL450});
 fragment.AddType({spv::OpTypeVoid,fv}); fragment.AddType({spv::OpTypeFloat,ff,32});
 fragment.AddType({spv::OpTypeVector,fvec,ff,4});
 fragment.AddType({spv::OpTypePointer,fp,spv::StorageClassOutput,fvec});
 fragment.AddType({spv::OpVariable,fp,output,spv::StorageClassOutput});
 const auto input_pointer=fragment.AllocateId(), input_color=fragment.AllocateId(), input_modulation=fragment.AllocateId();
 fragment.AddType({spv::OpTypePointer,input_pointer,spv::StorageClassInput,fvec});
 fragment.AddType({spv::OpVariable,input_pointer,input_color,spv::StorageClassInput});
 fragment.AddType({spv::OpVariable,input_pointer,input_modulation,spv::StorageClassInput});
 fragment.AddAnnotation({spv::OpDecorate,input_color,spv::DecorationLocation,0});
 fragment.AddAnnotation({spv::OpDecorate,input_modulation,spv::DecorationLocation,1});
 fragment.AddType({spv::OpTypeFunction,fft,fv});
 fragment.AddType({spv::OpConstant,ff,zero,0}); fragment.AddType({spv::OpConstant,ff,one,0x3f800000u});
 fragment.AddType({spv::OpConstantComposite,fvec,red,one,zero,zero,one});
 fragment.AddAnnotation({spv::OpDecorate,output,spv::DecorationLocation,0});
 fragment.AddEntryPoint(spv::ExecutionModelFragment,fm,"main",{output,input_color,input_modulation});
 fragment.AddExecutionMode({fm,spv::ExecutionModeOriginUpperLeft});
 fragment.AddFunction({spv::OpFunction,fv,fm,spv::FunctionControlMaskNone,fft});
 fragment.AddFunction({spv::OpLabel,fragment.AllocateId()});
 const auto loaded_color=fragment.AllocateId(), loaded_modulation=fragment.AllocateId(), shaded=fragment.AllocateId();
 fragment.AddFunction({spv::OpLoad,fvec,loaded_color,input_color});
 fragment.AddFunction({spv::OpLoad,fvec,loaded_modulation,input_modulation});
 fragment.AddFunction({spv::OpFMul,fvec,shaded,loaded_color,loaded_modulation});
 fragment.AddFunction({spv::OpStore,output,shaded}); fragment.AddFunction({spv::OpReturn});
 fragment.AddFunction({spv::OpFunctionEnd});
 const auto fragment_words=fragment.Build();
 file=std::fopen("mesh_fragment.spv","wb"); if(!file) return 4;
 const bool fragment_ok=std::fwrite(fragment_words.data(),sizeof(uint32_t),fragment_words.size(),file)==fragment_words.size();
 if(std::fclose(file)!=0 || !fragment_ok) return 5;
 std::puts("Mesh and fragment modules emitted; GPU execution not tested");
}
