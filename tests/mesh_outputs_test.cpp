#include "graphics/shader/recompiler/spirvEmitter/meshOutputs.h"
#include <cstdio>
#include <map>
using namespace Libs::Graphics::ShaderRecompiler::Spirv;
int main() {
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
 b.AddEntryPoint(spv::ExecutionModelMeshEXT,main,"main",{out.position,out.triangles,out.cull});
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
 }
 StoreMeshPackedTriangle(b,out,constant(0),constant((1u<<10)|(2u<<20)),constant);
 b.AddFunction({spv::OpReturn}); b.AddFunction({spv::OpFunctionEnd});
 const auto words=b.Build();
 if(words[1]!=0x00010400u || Builder{}.Build()[1]!=0x00010300u) return 1;
 auto* file=std::fopen("mesh_outputs.spv","wb"); if(!file) return 2;
 const bool ok=std::fwrite(words.data(),sizeof(uint32_t),words.size(),file)==words.size();
 if(std::fclose(file)!=0 || !ok) return 3;
 std::puts("Mesh output interface emitted; GPU execution not tested");
}
