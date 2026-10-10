#include "graphics/shader/recompiler/spirvEmitter/guestAddressRead.h"
#include <functional>
#include <unordered_set>
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/SpirvBuilder.h"
#include "graphics/shader/recompiler/spirvEmitter/guestAddressLookup.h"
#include "graphics/host_gpu/renderer/guestAddressTable.h"
#include <unordered_map>
#include <stdexcept>
#include <cstdio>
#include <cstring>
using namespace Libs::Graphics::ShaderRecompiler::Spirv;
using namespace Libs::Graphics;
struct Recorder {
 uint32_t next=1;
 std::vector<std::vector<uint32_t>> types,code;
 uint32_t AllocateId(){return next++;}
 void AddType(std::initializer_list<uint32_t> x){types.emplace_back(x);}
 void AddFunction(std::initializer_list<uint32_t> x){code.emplace_back(x);}
 void AddFunction(const std::vector<uint32_t>& x){code.push_back(x);}
 void AddName(uint32_t,const char*){}
 uint64_t Run(uint64_t guest,uint32_t bytes,const std::vector<uint32_t>& memory,
  std::function<uint64_t(uint64_t,uint32_t)> lookup = {},
  const std::unordered_map<uint64_t,uint32_t>* physical = nullptr, size_t* reads = nullptr) {
  std::unordered_set<uint32_t> physical_pointers;
  std::unordered_map<uint32_t,uint64_t> v;
  for(auto& x:types) {
   if(x[0]==43)v[x[2]]=x[3];
   if(x[0]==44)v[x[2]]=v[x[3]]|(v[x[4]]<<32);
  }
  uint32_t parameter=0;
  std::unordered_map<uint32_t,size_t> labels;
  for(size_t i=0;i<code.size();++i) {
   auto& x=code[i];
   if(x[0]==55)v[x[2]]=parameter++==0?guest:bytes;
   if(x[0]==248)labels[x[1]]=i;
  }
  uint32_t block=0,previous=0;
  for(size_t ip=0;ip<code.size();++ip) {
   const auto& x=code[ip];
   auto a=[&](size_t n){return uint32_t(v.at(x[n]));};
   switch(x[0]) {
    case 54:case 55:case 56:case 247:break;
    case 248:block=x[1];break;
    case 249:previous=block;ip=labels.at(x[1])-1;break;
    case 250:previous=block;ip=labels.at(v.at(x[1])?x[2]:x[3])-1;break;
    case 254:return v.at(x[1]);
    case 245:v[x[2]]=v.at(previous==x[4]?x[3]:x[5]);break;
    case 43:throw std::runtime_error("constant in function");
    case 68:v[x[2]]=memory.size();break;
    case 65:v[x[2]]=a(5);break;
    case 57:if(!lookup)throw std::runtime_error("missing lookup function");
            v[x[2]]=lookup(v.at(x[4]),a(5));break;
    case 120:v[x[2]]=v.at(x[3]);physical_pointers.insert(x[2]);break;
    case 61:if(physical_pointers.contains(x[3])) {
              if(!physical || x.size()!=6 || x[4]!=2 || x[5]!=4)
                throw std::runtime_error("invalid physical read contract");
              v[x[2]]=physical->at(v.at(x[3]));if(reads)++*reads;
            } else {
              if(a(3)>=memory.size())throw std::runtime_error("out of range table load");
              v[x[2]]=memory[a(3)];
            } break;
    case 113:v[x[2]]=a(3);break;
    case 196:v[x[2]]=v.at(x[3])<<a(4);break;
    case 197:v[x[2]]=v.at(x[3])|v.at(x[4]);break;
    case 199:v[x[2]]=a(3)&a(4);break;
    case 80:v[x[2]]=v.at(x[3])|(v.at(x[4])<<32);break;
    case 81:v[x[2]]=uint32_t(v.at(x[3])>>(32*x[4]));break;
    case 128:v[x[2]]=uint32_t(a(3)+a(4));break;
    case 130:v[x[2]]=uint32_t(a(3)-a(4));break;
    case 132:v[x[2]]=uint32_t(a(3)*a(4));break;
    case 134:v[x[2]]=a(3)/a(4);break;
    case 137:v[x[2]]=a(3)%a(4);break;
    case 149:v[x[2]]=uint64_t(a(3))+a(4);break;
    case 150:v[x[2]]=uint32_t(a(3)-a(4))|(uint64_t(a(3)<a(4))<<32);break;
    case 166:v[x[2]]=bool(a(3))||bool(a(4));break;
    case 167:v[x[2]]=bool(a(3))&&bool(a(4));break;
    case 169:v[x[2]]=a(3)?v.at(x[4]):v.at(x[5]);break;
    case 170:v[x[2]]=a(3)==a(4);break;
    case 171:v[x[2]]=a(3)!=a(4);break;
    case 176:v[x[2]]=a(3)<a(4);break;
    case 178:v[x[2]]=a(3)<=a(4);break;
    case 194:v[x[2]]=a(3)>>a(4);break;
    default:throw std::runtime_error("unexpected opcode");
   }
  }
  throw std::runtime_error("no return");
 }
};
int main() {
 Libs::Graphics::ShaderRecompiler::IR::Program plan;
 plan.user_data_count=0;plan.shader_info_complete=true;plan.uses_guest_address_table=true;
 Libs::Graphics::ShaderRecompiler::IR::BindingLayoutOptions options;
 std::string error;
 if(!Libs::Graphics::ShaderRecompiler::IR::AllocateBindings(&plan,options,&error) ||
    !Libs::Graphics::ShaderRecompiler::IR::FindBinding(plan.bindings,
      Libs::Graphics::ShaderRecompiler::IR::DescriptorBindingKind::GuestAddressTable))
  throw std::runtime_error("guest table descriptor missing");

 Recorder r;DefineGuestAddressLookup(r,100000,100001,100002,100003,100004,100005);
 std::vector<GuestAddressRange> ranges;
 for(uint32_t i=0;i<65536;++i)
  ranges.push_back({0x26500000000ull+uint64_t(i)*32768,
                    0x80000000ull+uint64_t(i)*65536,16384,0});
 std::vector<uint32_t> words(ranges.size()*8);
 std::memcpy(words.data(),ranges.data(),ranges.size()*sizeof(GuestAddressRange));
 auto check=[&](uint64_t address,uint32_t bytes) {
  uint64_t expected=0;ResolveGuestAddress(ranges,address,bytes,&expected);
  const auto actual=r.Run(address,bytes,words);
  if(actual!=expected)throw std::runtime_error("GPU lookup disagrees with CPU table resolver");
 };
 for(auto i:{0u,1u,32767u,32768u,65535u}) {
  auto base=ranges[i].guest_base;
  for(auto offset:{0u,4u,16320u,16380u,16384u,32767u})
   for(auto bytes:{0u,4u,64u,UINT32_MAX})check(base+offset,bytes);
 }
 uint64_t seed=33;
 for(uint32_t i=0;i<2000;++i) {
  seed=seed*6364136223846793005ull+1;
  check(ranges[uint32_t(seed)%65536].guest_base+((seed>>32)&32767),uint32_t(seed>>16)%129);
 }
 check(0,4);check(UINT64_MAX,4);check(GuestAddressLimit,4);
 if(r.Run(1,4,{})!=0 || r.Run(1,4,{1})!=0)
  throw std::runtime_error("invalid table accepted");
 Recorder reader;
 DefineGuestAddressRead(reader,100000,100006,100001,100002,100007,100008);
 std::unordered_map<uint64_t,uint32_t> physical;
 size_t reads=0;
 auto lookup_call=[&](uint64_t guest,uint32_t bytes){return r.Run(guest,bytes,words);};
 for(auto i:{0u,32768u,65535u}) {
  auto guest=ranges[i].guest_base+16380,device=ranges[i].device_base+16380;
  physical[device]=i==0?0u:0xabcdef12u;
  auto result=reader.Run(guest,4,{},lookup_call,&physical,&reads);
  if(result!=physical[device])throw std::runtime_error("physical load mismatch");
 }
 if(reads!=3)throw std::runtime_error("physical read count mismatch");
 auto before=reads;
 if(reader.Run(ranges[0].guest_base+16384,4,{},lookup_call,&physical,&reads)!=(1ull<<32) ||
    reader.Run(ranges[0].guest_base+1,4,{},lookup_call,&physical,&reads)!=(2ull<<32) ||
    reader.Run(ranges[0].guest_base,4,{},[](uint64_t,uint32_t){return 1ull;},&physical,&reads)!=(2ull<<32) ||
    reads!=before)throw std::runtime_error("rejected address was physically accessed");
 // Emit the same production helper into a real SPIR-V compute module.
 Builder b;
 auto u=b.AllocateId(),bo=b.AllocateId(),pair=b.AllocateId(),carry=b.AllocateId(),
      ptr=b.AllocateId(),array=b.AllocateId(),structure=b.AllocateId(),
      ptr_struct=b.AllocateId(),table=b.AllocateId(),vo=b.AllocateId(),
      main_type=b.AllocateId(),main=b.AllocateId(),label=b.AllocateId();
 b.AddCapability({1});b.AddCapability({11});b.AddCapability({5347});
 b.AddExtension("SPV_KHR_physical_storage_buffer");b.AddMemoryModel({5348,1});
 b.AddEntryPoint(5,main,"main",{});b.AddExecutionMode({main,17,1,1,1});
 b.AddAnnotation({71,array,6,4});b.AddAnnotation({71,structure,2});
 b.AddAnnotation({72,structure,0,35,0});
 b.AddAnnotation({71,table,33,0});b.AddAnnotation({71,table,34,0});
 b.AddType({21,u,32,0});b.AddType({20,bo});b.AddType({23,pair,u,2});
 b.AddType({30,carry,u,u});b.AddType({29,array,u});b.AddType({30,structure,array});
 b.AddType({32,ptr,12,u});b.AddType({32,ptr_struct,12,structure});
 b.AddType({59,ptr_struct,table,12});b.AddType({19,vo});b.AddType({33,main_type,vo});
 const auto lookup=DefineGuestAddressLookup(b,u,bo,pair,carry,ptr,table);
 const auto u64=b.AllocateId(),physical_ptr=b.AllocateId();
 b.AddType({21,u64,64,0});b.AddType({32,physical_ptr,5349,u});
 const auto read=DefineGuestAddressRead(b,u,u64,bo,pair,physical_ptr,lookup);
 const auto z=b.AllocateId(),four=b.AllocateId(),input=b.AllocateId(),call=b.AllocateId();
 b.AddType({43,u,z,0});b.AddType({43,u,four,4});b.AddType({44,pair,input,z,z});
 b.AddFunction({54,vo,main,0,main_type});b.AddFunction({248,label});
 b.AddFunction({57,pair,call,read,input});
 b.AddFunction({253});b.AddFunction({56});
 const auto binary=b.Build();
 auto f=std::fopen("guest_address_lookup.spv","wb");if(!f)return 1;
 std::fwrite(binary.data(),4,binary.size(),f);std::fclose(f);
 std::puts("PASS: production SPIR-V lookup agrees with CPU resolver, including 65536 entries and 2000 random accesses; checked physical loads never access missing/unaligned backing");
}
