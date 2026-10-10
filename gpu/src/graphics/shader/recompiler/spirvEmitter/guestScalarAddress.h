#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// Dynamic offset and component byte offset are independently zero-extended:
// their sum must not wrap at 32 bits. Noncanonical bases remain invalid rather
// than wrapping into an unrelated valid range during 64-bit addition.
template<class B>
uint32_t EmitGuestScalarAddress(B& b,uint32_t u32,uint32_t u64,uint32_t boolean,
                               uint32_t pair,uint32_t low,uint32_t high,
                               uint32_t dynamic_offset,uint32_t component_offset) {
 auto constant=[&](uint32_t n){auto id=b.AllocateId();b.AddType({43,u32,id,n});return id;};
 auto op=[&](uint32_t code,uint32_t type,std::initializer_list<uint32_t> args) {
  auto id=b.AllocateId();std::vector<uint32_t> words{code,type,id};
  words.insert(words.end(),args.begin(),args.end());b.AddFunction(words);return id;
 };
 auto bits=constant(32),limit=constant(65536),aligned=constant(~3u);
 dynamic_offset=op(199,u32,{dynamic_offset,aligned});
 component_offset=op(199,u32,{component_offset,aligned});
 auto base=op(197,u64,{op(113,u64,{low}),op(196,u64,{op(113,u64,{high}),bits})});
 auto offset=op(128,u64,{op(113,u64,{dynamic_offset}),op(113,u64,{component_offset})});
 auto address=op(128,u64,{base,offset});
 auto valid=op(176,boolean,{high,limit});
 auto result_low=op(169,u32,{valid,op(113,u32,{address}),low});
 auto result_high=op(169,u32,{valid,op(113,u32,{op(194,u64,{address,bits})}),high});
 return op(80,pair,{result_low,result_high});
}
}
