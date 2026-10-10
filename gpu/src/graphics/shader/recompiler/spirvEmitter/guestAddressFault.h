#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// Four DWORDs: status, guest low, guest high, reserved. Cleared before dispatch.
// Read ONLY after the dispatch fence: status is claimed before address stores.
// Atomic ownership prevents competing invocations from mixing address halves.
template<class B>
uint32_t DefineGuestAddressFault(B& b, uint32_t u32, uint32_t boolean,
                                uint32_t pair, uint32_t storage_uint_ptr,
                                uint32_t fault_buffer) {
 auto constant=[&](uint32_t n){auto id=b.AllocateId();b.AddType({43,u32,id,n});return id;};
 auto zero=constant(0),one=constant(1),two=constant(2);
 auto type=b.AllocateId();b.AddType({33,type,u32,pair,u32});
 auto fn=b.AllocateId(),guest=b.AllocateId(),status=b.AllocateId(),entry=b.AllocateId();
 b.AddFunction({54,u32,fn,0,type});b.AddFunction({55,pair,guest});
 b.AddFunction({55,u32,status});b.AddFunction({248,entry});
 auto op=[&](uint32_t code,uint32_t type,std::initializer_list<uint32_t> args) {
  auto id=b.AllocateId();std::vector<uint32_t> words{code,type,id};
  words.insert(words.end(),args.begin(),args.end());b.AddFunction(words);return id;
 };
 auto pointer=[&](uint32_t index){return op(65,storage_uint_ptr,{fault_buffer,zero,index});};
 auto merge=b.AllocateId(),claim=b.AllocateId();
 b.AddFunction({247,merge,0});
 b.AddFunction({250,op(171,boolean,{status,zero}),claim,merge});
 b.AddFunction({248,claim});
 auto old=op(230,u32,{pointer(zero),one,zero,zero,status,zero});
 auto write=b.AllocateId(),finish=b.AllocateId();
 b.AddFunction({247,finish,0});b.AddFunction({250,op(170,boolean,{old,zero}),write,finish});
 b.AddFunction({248,write});
 auto lo=op(81,u32,{guest,0}),hi=op(81,u32,{guest,1});
 b.AddFunction({62,pointer(one),lo});b.AddFunction({62,pointer(two),hi});
 b.AddFunction({249,finish});b.AddFunction({248,finish});
 b.AddFunction({249,merge});b.AddFunction({248,merge});
 b.AddFunction({254,status});b.AddFunction({56});
 b.AddName(fn,"record_first_guest_address_fault");
 return fn;
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv
