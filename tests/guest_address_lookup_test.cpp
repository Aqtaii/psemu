#include "graphics/shader/recompiler/spirvEmitter/guestScalarBufferRead.h"
#include "graphics/shader/recompiler/spirvEmitter/guestAddressFault.h"
#include "graphics/shader/recompiler/SrtPatcher.h"
#include "graphics/shader/recompiler/spirvEmitter/scalarMemoryGroup.h"
#include "graphics/shader/recompiler/spirvEmitter/guestAddressRead.h"
#include "graphics/shader/recompiler/ResourceTracking.h"
#include "graphics/shader/recompiler/spirvEmitter/guestScalarAddress.h"
#include "graphics/shader/recompiler/spirvEmitter/guestBvhIntersect.h"
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
  const std::unordered_map<uint64_t,uint32_t>* physical = nullptr, size_t* reads = nullptr,
  std::function<void(uint64_t,uint32_t)> fault = {},
  std::vector<uint32_t>* fault_words = nullptr) {
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
    case 57:if(x[3]==100009) {
              if(!fault)throw std::runtime_error("missing fault reporter");
              fault(v.at(x[4]),a(5));v[x[2]]=a(5);break;
            }
            if(!lookup)throw std::runtime_error("missing lookup function");
            v[x[2]]=lookup(v.at(x[4]),a(5));break;
    case 230:if(!fault_words || a(4)!=1 || a(5)!=0 || a(6)!=0)
               throw std::runtime_error("invalid fault atomic");
             v[x[2]]=fault_words->at(a(3));
             if(v[x[2]]==a(8))fault_words->at(a(3))=a(7);
             break;
    case 62:if(!fault_words)throw std::runtime_error("fault store without backing");
            fault_words->at(a(1))=a(2);break;
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
    case 128:v[x[2]]=x[1]==100006?v.at(x[3])+v.at(x[4]):uint32_t(a(3)+a(4));break;
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
    case 194:v[x[2]]=x[1]==100006?v.at(x[3])>>a(4):a(3)>>a(4);break;
    default:throw std::runtime_error("unexpected opcode");
   }
  }
  throw std::runtime_error("no return");
 }
};
#include "guest_bvh_semantic_test.h"
int main() {
 TestGuestBvhSemantics();
 TestGuestScalarBufferSemantics();
 namespace IR=Libs::Graphics::ShaderRecompiler::IR;
 auto group_plan=[](bool all_static) {
  IR::Program p;p.srt_plan_complete=true;p.blocks.resize(1);
  p.provenance.values.resize(6);
  for(uint32_t i=0;i<4;++i) {
   p.provenance.values[i+2].op=IR::ScalarValueOp::ReadConst;
   IR::Instruction inst;inst.op=IR::Opcode::SLoadDword;inst.pc=0x2030;
   inst.dst.kind=IR::OperandKind::Register;inst.dst.reg={IR::RegisterFile::Scalar,8+i};
   inst.src_count=1;inst.src[0].kind=IR::OperandKind::Register;
   inst.src[0].reg={IR::RegisterFile::Scalar,12};
   inst.scalar_value=i+2;inst.memory.kind=IR::ResourceKind::ScalarBuffer;
   inst.memory.resource=8;inst.memory.component_count=4;inst.memory.component_index=i;
   inst.memory.offset=i*4;p.blocks[0].instructions.push_back(inst);
   if(all_static || i==0 || i==3)
    p.srt.reads.push_back({i+2,uint32_t(p.srt.reads.size()),0x2030});
  }
  return p;
 };
 std::string patch_error;
 auto partial=group_plan(false);
 if(!IR::PatchSrtReads(&partial,&patch_error))throw std::runtime_error(patch_error);
 if(partial.blocks[0].instructions[0].op!=IR::Opcode::LoadSrtDword ||
    partial.blocks[0].instructions[1].op!=IR::Opcode::SLoadDword)
  throw std::runtime_error("partial patch regression");
 for(uint32_t i=0;i<4;++i) {
  auto& group=partial.blocks[0].instructions[i].scalar_memory_group;
  if(!group.valid || group.base_sgpr!=8 || group.component_index!=i ||
     group.component_count!=4 || group.dynamic_offset.reg.index!=12)
   throw std::runtime_error("scalar group address metadata lost");
 }
 std::array<uint32_t,128> regs{};regs[8]=0xfffffffc;regs[9]=0x123;regs[12]=4;
 const auto snapshot=CaptureScalarMemoryGroup(
  partial.blocks[0].instructions[0].scalar_memory_group,
  [&](const IR::Operand& source){return regs.at(source.reg.index);});
 regs[8]=7;regs[9]=8;regs[12]=9;
 if(snapshot!=std::array<uint32_t,3>{0xfffffffc,0x123,4})
  throw std::runtime_error("overlapping writes changed captured address");
 auto fixed=group_plan(true);
 if(!IR::PatchSrtReads(&fixed,&patch_error))throw std::runtime_error(patch_error);
 for(auto& inst:fixed.blocks[0].instructions)
  if(inst.scalar_memory_group.valid)throw std::runtime_error("static read captured unnecessarily");
 auto malformed=group_plan(false);
 malformed.blocks[0].instructions[3].memory.resource=10;
 if(IR::PatchSrtReads(&malformed,&patch_error))throw std::runtime_error("malformed group accepted");
 for(auto& inst:malformed.blocks[0].instructions)
  if(inst.op!=IR::Opcode::SLoadDword || inst.scalar_memory_group.valid)
   throw std::runtime_error("group failure partially modified program");

 partial.uses_guest_address_table=true;
 if(!IR::TrackResources(&partial,&patch_error))throw std::runtime_error(patch_error);
 if(!partial.uses_guest_address_table || !partial.info.addresses.empty() ||
    !partial.blocks[0].instructions[1].memory.guest_address_read)
  throw std::runtime_error("GPU scalar load incorrectly materialized as CPU descriptor");
 auto rollback=group_plan(false);rollback.uses_guest_address_table=true;
 if(!IR::PatchSrtReads(&rollback,&patch_error))throw std::runtime_error(patch_error);
 rollback.blocks[0].instructions[2].memory.typed=true;
 if(IR::TrackResources(&rollback,&patch_error))throw std::runtime_error("typed scalar fallback accepted");
 for(auto& inst:rollback.blocks[0].instructions)
  if(inst.memory.guest_address_read)throw std::runtime_error("tracking failure partially patched");
 auto sb=group_plan(false);sb.blocks[0].instructions.resize(1);sb.srt.reads.clear();
 sb.blocks[0].instructions[0].op=IR::Opcode::SBufferLoadDword;
 sb.blocks[0].instructions[0].memory.component_count=1;
 sb.blocks[0].instructions[0].memory.resource=2;
 sb.blocks[0].instructions[0].memory.resource_source=IR::ScalarProvenance::Unknown;
 sb.uses_guest_address_table=true;
 if(!IR::PatchSrtReads(&sb,&patch_error) || !IR::TrackResources(&sb,&patch_error) ||
    !sb.blocks[0].instructions[0].memory.guest_buffer_read || !sb.info.buffers.empty())
  throw std::runtime_error("runtime scalar buffer was not kept on GPU");
 for(uint32_t count:{2u,4u,8u,16u}) {
  for(bool partial_patch:{false,true}) {
   auto wide_sb=group_plan(false);wide_sb.srt.reads.clear();
   auto prototype=wide_sb.blocks[0].instructions[0];
   wide_sb.blocks[0].instructions.clear();wide_sb.provenance.values.resize(count+2);
   for(uint32_t i=0;i<count;++i) {
    auto inst=prototype;inst.op=IR::Opcode::SBufferLoadDword;
    inst.dst.reg.index=32+i;inst.scalar_value=i+2;
    inst.memory.component_count=count;inst.memory.component_index=i;inst.memory.offset=i*4;
    wide_sb.provenance.values[i+2].op=IR::ScalarValueOp::ReadConstBuffer;
    wide_sb.blocks[0].instructions.push_back(inst);
    if(partial_patch && i==0)wide_sb.srt.reads.push_back({i+2,0,0x2030});
   }
   wide_sb.uses_guest_address_table=true;
   if(!IR::PatchSrtReads(&wide_sb,&patch_error) || !IR::TrackResources(&wide_sb,&patch_error))
    throw std::runtime_error("wide scalar descriptor fallback rejected: "+patch_error);
   for(uint32_t i=0;i<count;++i) {
    auto& inst=wide_sb.blocks[0].instructions[i];
    if(inst.scalar_memory_group.descriptor_dwords!=4 ||
       inst.scalar_memory_group.base_sgpr!=32 ||
       inst.scalar_memory_group.component_count!=count ||
       inst.scalar_memory_group.component_index!=i ||
       (!(partial_patch && i==0) && !inst.memory.guest_buffer_read))
     throw std::runtime_error("wide scalar descriptor metadata lost");
   }
   std::array<uint32_t,128> buffer_regs{};
   buffer_regs[32]=1;buffer_regs[33]=2;buffer_regs[34]=3;buffer_regs[35]=4;buffer_regs[12]=5;
   auto buffer_snapshot=CaptureScalarBufferGroup(wide_sb.blocks[0].instructions[0].scalar_memory_group,
    [&](const IR::Operand& source){return buffer_regs.at(source.reg.index);});
   buffer_regs.fill(0);
   if(buffer_snapshot!=std::array<uint32_t,5>{1,2,3,4,5})
    throw std::runtime_error("buffer descriptor/address changed after overlapping write");
  }
 }
 auto legacy=group_plan(false);
 if(!IR::PatchSrtReads(&legacy,&patch_error))throw std::runtime_error(patch_error);
 if(IR::TrackResources(&legacy,&patch_error))throw std::runtime_error("disabled fallback accepted unknown address");
 auto address_check=[](uint64_t base,uint32_t offset,uint32_t component,uint64_t expected) {
  Recorder address;
  auto low=address.AllocateId(),high=address.AllocateId(),parameter=address.AllocateId(),
       addend=address.AllocateId();
  address.AddFunction({55,100002,parameter});
  address.AddFunction({81,100000,low,parameter,0});
  address.AddFunction({81,100000,high,parameter,1});
  address.AddType({43,100000,addend,component});
  auto dynamic=address.AllocateId();address.AddType({43,100000,dynamic,offset});
  auto value=EmitGuestScalarAddress(address,100000,100006,100001,100002,
                                    low,high,dynamic,addend);
  address.AddFunction({254,value});
  if(address.Run(base,4,{})!=expected)throw std::runtime_error("scalar address carry/overflow regression");
 };
 address_check(0x123fffffffcull,4,0,0x12400000000ull);
 address_check(0x12300000000ull,UINT32_MAX,60,0x12400000038ull);
 address_check(0x123fffffffcull,7,3,0x12400000000ull);
 address_check(UINT64_MAX-3,8,0,UINT64_MAX-3);
 address_check(GuestAddressLimit-4,4,0,GuestAddressLimit);
 Libs::Graphics::ShaderRecompiler::IR::Program plan;
 plan.user_data_count=0;plan.shader_info_complete=true;plan.uses_guest_address_table=true;
 Libs::Graphics::ShaderRecompiler::IR::BindingLayoutOptions options;
 std::string error;
 if(!Libs::Graphics::ShaderRecompiler::IR::AllocateBindings(&plan,options,&error) ||
    !Libs::Graphics::ShaderRecompiler::IR::FindBinding(plan.bindings,
      Libs::Graphics::ShaderRecompiler::IR::DescriptorBindingKind::GuestAddressTable) ||
    !Libs::Graphics::ShaderRecompiler::IR::FindBinding(plan.bindings,
      Libs::Graphics::ShaderRecompiler::IR::DescriptorBindingKind::GuestAddressFault))
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
 // Exercise the actual first-fault helper and the reader's error callbacks.
 Recorder reporter;
 DefineGuestAddressFault(reporter,100000,100001,100002,100004,100010);
 std::vector<uint32_t> fault_words(4);
 auto report=[&](uint64_t guest,uint32_t status) {
  reporter.Run(guest,status,{}, {},nullptr,nullptr,{},&fault_words);
 };
 report(0x123456789abcull,0);
 if(fault_words!=std::vector<uint32_t>(4))throw std::runtime_error("success reported as fault");
 report(0x123456789abcull,1);
 report(0xabcdef123456ull,2);
 if(fault_words!=std::vector<uint32_t>{1,0x56789abc,0x1234,0})
  throw std::runtime_error("first fault overwritten or torn");
 Recorder reporting_reader;
 DefineGuestAddressRead(reporting_reader,100000,100006,100001,100002,100007,100008,100009);
 size_t reports=0;
 auto counted_report=[&](uint64_t guest,uint32_t status){++reports;report(guest,status);};
 fault_words.assign(4,0);
 reporting_reader.Run(ranges[0].guest_base+16380,4,{},lookup_call,&physical,&reads,counted_report);
 if(reports || fault_words[0])throw std::runtime_error("valid zero payload reported");
 auto missing_guest=ranges[0].guest_base+16384;
 reporting_reader.Run(missing_guest,4,{},lookup_call,&physical,&reads,counted_report);
 if(reports!=1 || fault_words[0]!=1 ||
    (uint64_t(fault_words[1])|(uint64_t(fault_words[2])<<32))!=missing_guest)
  throw std::runtime_error("missing read address not reported");
 fault_words.assign(4,0);
 reporting_reader.Run(missing_guest+1,4,{},lookup_call,&physical,&reads,counted_report);
 if(reports!=2 || fault_words[0]!=2 ||
    (uint64_t(fault_words[1])|(uint64_t(fault_words[2])<<32))!=missing_guest+1)
  throw std::runtime_error("unaligned read address not reported");
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
 const auto fault_buffer=b.AllocateId();
 b.AddType({59,ptr_struct,fault_buffer,12});
 b.AddAnnotation({71,fault_buffer,33,1});b.AddAnnotation({71,fault_buffer,34,0});
 const auto report_fn=DefineGuestAddressFault(b,u,bo,pair,ptr,fault_buffer);
 const auto read=DefineGuestAddressRead(b,u,u64,bo,pair,physical_ptr,lookup,report_fn);
 const auto float_type=b.AllocateId(),vec2f=b.AllocateId(),vec3f=b.AllocateId(),
            vec4u=b.AllocateId(),glsl=b.AllocateId();
 b.AddType({22,float_type,32});b.AddType({23,vec2f,float_type,2});
 b.AddType({23,vec3f,float_type,3});b.AddType({23,vec4u,u,4});
 b.AddExtInstImport(glsl,"GLSL.std.450");
 const auto scalar_buffer=DefineGuestScalarBufferRead(b,u,u64,bo,pair,vec4u,read);
 const auto bvh=Bvh::Define(b,u,float_type,bo,u64,pair,vec4u,vec2f,vec3f,
                            physical_ptr,glsl,lookup,report_fn);
 const auto z=b.AllocateId(),four=b.AllocateId(),input=b.AllocateId(),call=b.AllocateId();
 b.AddType({43,u,z,0});b.AddType({43,u,four,4});b.AddType({44,pair,input,z,z});
 b.AddFunction({54,vo,main,0,main_type});b.AddFunction({248,label});
 const auto scalar_address=EmitGuestScalarAddress(b,u,u64,bo,pair,z,z,four,z);
 b.AddFunction({57,pair,call,read,scalar_address});
 const auto descriptor=b.AllocateId(),node=b.AllocateId(),extent=b.AllocateId(),
            origin=b.AllocateId(),bvh_call=b.AllocateId();
 b.AddType({44,vec4u,descriptor,z,z,z,z});b.AddType({43,u64,node,0,0});
 b.AddType({43,float_type,extent,0});b.AddType({44,vec3f,origin,extent,extent,extent});
 b.AddFunction({57,vec4u,bvh_call,bvh,descriptor,node,extent,origin,origin,origin});
 auto sbcall=b.AllocateId();b.AddFunction({57,pair,sbcall,scalar_buffer,descriptor,z,z});
 b.AddFunction({253});b.AddFunction({56});
 const auto binary=b.Build();
 auto f=std::fopen("guest_address_lookup.spv","wb");if(!f)return 1;
 std::fwrite(binary.data(),4,binary.size(),f);std::fclose(f);
 std::puts("PASS: production SPIR-V lookup agrees with CPU resolver, including 65536 entries and 2000 random accesses; checked physical loads never access missing/unaligned backing");
}
