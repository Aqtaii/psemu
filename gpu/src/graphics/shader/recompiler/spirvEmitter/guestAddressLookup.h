#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// Emits a callable function: uvec2 lookup(uvec2 guest, uint byte_count).
// Uses the serialized 8-DWORD GuestAddressRange layout and only 32-bit arithmetic.
// Returns {0,0} on a missing/boundary/invalid access. The caller must diagnose this
// result before attempting a physical-storage-buffer read.
template<class B>
uint32_t DefineGuestAddressLookup(B& b, uint32_t u32, uint32_t boolean, uint32_t pair,
                                 uint32_t carry_pair, uint32_t ptr_uint, uint32_t table) {
 auto constant=[&](uint32_t v){auto id=b.AllocateId();b.AddType({43,u32,id,v});return id;};
 const auto zero=constant(0),one=constant(1),eight=constant(8),limit=constant(65536),
            high_limit=constant(65536);
 const auto null_pair=b.AllocateId();b.AddType({44,pair,null_pair,zero,zero});
 const auto type=b.AllocateId();b.AddType({33,type,pair,pair,u32});
 const auto fn=b.AllocateId(),guest=b.AllocateId(),bytes=b.AllocateId();
 b.AddFunction({54,pair,fn,0,type});
 b.AddFunction({55,pair,guest});b.AddFunction({55,u32,bytes});
 auto op=[&](uint32_t code,uint32_t type,std::initializer_list<uint32_t> args){
  auto id=b.AllocateId();std::vector<uint32_t> words{code,type,id};
  words.insert(words.end(),args.begin(),args.end());b.AddFunction(words);return id;
 };
 auto extract=[&](uint32_t v,uint32_t i){return op(81,u32,{v,i});};
 auto cmp=[&](uint32_t code,uint32_t a,uint32_t c){return op(code,boolean,{a,c});};
 auto both=[&](uint32_t a,uint32_t c){return op(167,boolean,{a,c});};
 auto either=[&](uint32_t a,uint32_t c){return op(166,boolean,{a,c});};
 auto select=[&](uint32_t c,uint32_t a,uint32_t d){return op(169,u32,{c,a,d});};
 auto add=[&](uint32_t a,uint32_t c){return op(128,u32,{a,c});};
 auto sub=[&](uint32_t a,uint32_t c){return op(130,u32,{a,c});};
 auto le_pair=[&](uint32_t lo,uint32_t hi,uint32_t rlo,uint32_t rhi){
  return either(cmp(176,hi,rhi),both(cmp(170,hi,rhi),cmp(178,lo,rlo)));
 };
 const auto entry=b.AllocateId();b.AddFunction({248,entry});
 const auto glo=extract(guest,0),ghi=extract(guest,1);
 const auto length=op(68,u32,{table,0}),count=op(134,u32,{length,eight});
 auto valid=both(cmp(171,count,zero),cmp(178,count,limit));
 valid=both(valid,cmp(176,ghi,high_limit));
 valid=both(valid,cmp(171,bytes,zero));
 valid=both(valid,cmp(170,op(137,u32,{length,eight}),zero));
 const auto active=b.AllocateId(),empty=b.AllocateId(),merge=b.AllocateId();
 b.AddFunction({247,merge,0});b.AddFunction({250,valid,active,empty});
 b.AddFunction({248,empty});b.AddFunction({249,merge});
 b.AddFunction({248,active});
 auto load=[&](uint32_t index,uint32_t word){
  auto base=op(132,u32,{index,eight});
  auto slot=add(base,constant(word));
  auto pointer=op(65,ptr_uint,{table,zero,slot});
  return op(61,u32,{pointer});
 };
 uint32_t left=zero,right=count;
 // upper_bound over at most 65536 entries needs at most 17 decisions.
 for(uint32_t i=0;i<17;++i) {
  auto mid=op(194,u32,{add(left,right),one});
  auto safe=select(cmp(176,mid,count),mid,sub(count,one));
  auto blo=load(safe,0),bhi=load(safe,1);
  auto advance=le_pair(blo,bhi,glo,ghi);
  auto searching=cmp(176,left,right);
  left=select(searching,select(advance,add(mid,one),left),left);
  right=select(searching,select(advance,right,mid),right);
 }
 auto candidate=select(cmp(171,left,zero),sub(left,one),zero);
 auto blo=load(candidate,0),bhi=load(candidate,1);
 auto dlo=load(candidate,2),dhi=load(candidate,3);
 auto size_lo=load(candidate,4),size_hi=load(candidate,5);
 auto difference=op(150,carry_pair,{glo,blo});
 auto offset_lo=extract(difference,0),borrow=extract(difference,1);
 auto offset_hi=sub(sub(ghi,bhi),borrow);
 auto end=op(149,carry_pair,{offset_lo,bytes});
 auto end_lo=extract(end,0),end_hi=add(offset_hi,extract(end,1));
 auto fits=both(cmp(171,left,zero),le_pair(blo,bhi,glo,ghi));
 fits=both(fits,le_pair(end_lo,end_hi,size_lo,size_hi));
 // Restrict guest range to 48 bits, including a read crossing its upper bound.
 fits=both(fits,either(cmp(176,end_hi,high_limit),
                      both(cmp(170,end_hi,high_limit),cmp(170,end_lo,zero))));
 auto device=op(149,carry_pair,{dlo,offset_lo});
 auto result=op(80,pair,{extract(device,0),add(add(dhi,offset_hi),extract(device,1))});
 auto selected=op(80,pair,{select(fits,extract(result,0),zero),
                           select(fits,extract(result,1),zero)});
 b.AddFunction({249,merge});
 b.AddFunction({248,merge});
 auto answer=op(245,pair,{null_pair,empty,selected,active});
 b.AddFunction({254,answer});b.AddFunction({56});
 b.AddName(fn,"resolve_guest_address");
 return fn;
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv
