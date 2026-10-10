#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// KytyPS5 LoadIndirectScalarBuffer semantics: aligned base/offset, widened
// max(stride,1)*records bounds. Architectural OOB is {0,success}; missing
// backing remains a distinct checked-read error.
template<class B>
uint32_t DefineGuestScalarBufferRead(B& b,uint32_t u,uint32_t wide,uint32_t boolean,
                                    uint32_t pair,uint32_t vec4,uint32_t checked_read) {
 auto c=[&](uint32_t v){auto id=b.AllocateId();b.AddType({43,u,id,v});return id;};
 auto zero=c(0),one=c(1),four=c(4),bits=c(32),mask=c(~3u),highmask=c(65535),shift=c(16),stride_mask=c(16383);
 auto empty=b.AllocateId();b.AddType({44,pair,empty,zero,zero});
 auto type=b.AllocateId();b.AddType({33,type,pair,vec4,u,u});
 auto fn=b.AllocateId(),desc=b.AllocateId(),offset=b.AllocateId(),component=b.AllocateId();
 b.AddFunction({54,pair,fn,0,type});b.AddFunction({55,vec4,desc});
 b.AddFunction({55,u,offset});b.AddFunction({55,u,component});
 auto label=b.AllocateId();b.AddFunction({248,label});
 auto op=[&](uint32_t code,uint32_t type,std::initializer_list<uint32_t> args) {
  auto id=b.AllocateId();std::vector<uint32_t> words{code,type,id};
  words.insert(words.end(),args.begin(),args.end());b.AddFunction(words);return id;
 };
 auto word=[&](unsigned i){return op(81,u,{desc,i});};
 auto low=op(199,u,{word(0),mask}),packed=word(1),records=word(2);
 auto high=op(199,u,{packed,highmask});
 auto stride=op(199,u,{op(194,u,{packed,shift}),stride_mask});
 stride=op(169,u,{op(170,boolean,{stride,zero}),one,stride});
 auto size=op(132,wide,{op(113,wide,{stride}),op(113,wide,{records})});
 auto byte=op(128,wide,{op(113,wide,{op(199,u,{offset,mask})}),
                        op(113,wide,{op(199,u,{component,mask})})});
 auto end=op(128,wide,{byte,op(113,wide,{four})});
 auto valid=op(178,boolean,{end,size});
 auto next=b.AllocateId(),oob=b.AllocateId();
 b.AddFunction({247,next,0});b.AddFunction({250,valid,next,oob});
 b.AddFunction({248,oob});b.AddFunction({254,empty});b.AddFunction({248,next});
 auto base=op(197,wide,{op(113,wide,{low}),op(196,wide,{op(113,wide,{high}),bits})});
 auto address=op(128,wide,{base,byte});
 auto guest=op(80,pair,{op(113,u,{address}),op(113,u,{op(194,wide,{address,bits})})});
 auto result=op(57,pair,{checked_read,guest});
 b.AddFunction({254,result});b.AddFunction({56});b.AddName(fn,"read_indirect_scalar_buffer_checked");
 return fn;
}
}
