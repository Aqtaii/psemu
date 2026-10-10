#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// Callable uvec2 read(uvec2 guest): {DWORD, status}. Status 0=success,
// 1=missing backing/boundary, 2=unaligned address. The caller must consume
// status and report missing backing; this must not become a silent zero read.
template<class B>
uint32_t DefineGuestAddressRead(B& b, uint32_t u32, uint32_t u64, uint32_t boolean,
                               uint32_t pair, uint32_t physical_uint_ptr,
                               uint32_t lookup) {
 auto constant=[&](uint32_t n){auto id=b.AllocateId();b.AddType({43,u32,id,n});return id;};
 auto zero=constant(0),one=constant(1),two=constant(2),three=constant(3),
      four=constant(4),bits=constant(32);
 auto missing=b.AllocateId(),unaligned=b.AllocateId();
 b.AddType({44,pair,missing,zero,one});b.AddType({44,pair,unaligned,zero,two});
 auto type=b.AllocateId();b.AddType({33,type,pair,pair});
 auto fn=b.AllocateId(),guest=b.AllocateId(),entry=b.AllocateId();
 b.AddFunction({54,pair,fn,0,type});b.AddFunction({55,pair,guest});b.AddFunction({248,entry});
 auto op=[&](uint32_t code,uint32_t type,std::initializer_list<uint32_t> args) {
  auto id=b.AllocateId();std::vector<uint32_t> words{code,type,id};
  words.insert(words.end(),args.begin(),args.end());b.AddFunction(words);return id;
 };
 auto extract=[&](uint32_t v,uint32_t index){return op(81,u32,{v,index});};
 // Each rejected branch returns before pointer conversion or OpLoad.
 auto guard=[&](uint32_t condition,uint32_t error) {
  auto next=b.AllocateId(),reject=b.AllocateId();
  b.AddFunction({247,next,0});b.AddFunction({250,condition,next,reject});
  b.AddFunction({248,reject});b.AddFunction({254,error});b.AddFunction({248,next});
 };
 auto lo=extract(guest,0);
 guard(op(170,boolean,{op(199,u32,{lo,three}),zero}),unaligned);
 auto device=op(57,pair,{lookup,guest,four});
 auto dlo=extract(device,0),dhi=extract(device,1);
 guard(op(171,boolean,{op(197,u32,{dlo,dhi}),zero}),missing);
 guard(op(170,boolean,{op(199,u32,{dlo,three}),zero}),unaligned);
 auto low64=op(113,u64,{dlo}),high64=op(113,u64,{dhi});
 auto address=op(197,u64,{low64,op(196,u64,{high64,bits})});
 auto pointer=op(120,physical_uint_ptr,{address});
 auto value=op(61,u32,{pointer,2,4}); // MemoryAccessAligned, alignment 4.
 auto result=op(80,pair,{value,zero});
 b.AddFunction({254,result});b.AddFunction({56});
 b.AddName(fn,"read_guest_dword_checked");
 return fn;
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv
