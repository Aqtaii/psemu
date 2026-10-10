#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"
namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
void EmitBvhIntersect(EmitterState* s,const IR::Instruction& inst) {
 EXIT_IF(!s->guest_bvh_function);
 // Snapshot all ray/descriptor operands before writing overlapping outputs.
 std::array<uint32_t,4> descriptor_values;
 for(unsigned i=0;i<4;++i) descriptor_values[i]=EmitValueLoad(s,inst.src[i]);
 auto descriptor=s->builder.AllocateId();
 s->builder.AddFunction({OpCompositeConstruct,s->vec4_uint_type,descriptor,
  descriptor_values[0],descriptor_values[1],descriptor_values[2],descriptor_values[3]});
 std::array<uint32_t,12> ray {};
 for(unsigned i=0;i<inst.bvh.ray.size();++i) ray[i]=EmitValueLoad(s,inst.bvh.ray[i]);
 auto low=s->builder.AllocateId(),high=s->builder.AllocateId(),shift=s->builder.AllocateId(),
      node=s->builder.AllocateId();
 s->builder.AddFunction({113,s->guest_uint64_type,low,ray[0]});
 s->builder.AddFunction({113,s->guest_uint64_type,high,
  inst.bvh.node_dwords==2?ray[1]:ConstantU32(s,0)});
 s->builder.AddFunction({OpShiftLeftLogical,s->guest_uint64_type,shift,high,ConstantU32(s,32)});
 s->builder.AddFunction({OpBitwiseOr,s->guest_uint64_type,node,low,shift});
 const auto n=inst.bvh.node_dwords;
 auto extent=EmitBitcastU32ToF32(s,ray[n]);
 auto vector=[&](unsigned first) {
  std::array<uint32_t,3> values;
  for(unsigned i=0;i<3;++i) values[i]=EmitBitcastU32ToF32(s,ray[first+i]);
  auto id=s->builder.AllocateId();
  s->builder.AddFunction({OpCompositeConstruct,s->vec3_float_type,id,values[0],values[1],values[2]});
  return id;
 };
 auto origin=vector(n+1),direction=vector(n+4),inverse=vector(n+7);
 EmitGuardedByExec(s,[&] {
  auto result=s->builder.AllocateId();
  s->builder.AddFunction({57,s->vec4_uint_type,result,s->guest_bvh_function,
                          descriptor,node,extent,origin,direction,inverse});
  std::array<uint32_t,4> values;
  for(unsigned i=0;i<4;++i) {
   values[i]=s->builder.AllocateId();
   s->builder.AddFunction({OpCompositeExtract,s->uint_type,values[i],result,i});
  }
  for(unsigned i=0;i<4;++i) {
   auto dst=inst.dst;dst.reg.index+=i;EmitStoreU32(s,dst,values[i]);
  }
 });
}
}
