#include <bit>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
enum {OpExtInst=1,OpBitcast,OpIAdd,OpINotEqual,OpSelect,GlslFindILsb=100};
namespace IR {struct Operand {uint64_t value=0;};struct Instruction {Operand src[1],dst;};}
struct Builder {
 uint32_t next=10;std::unordered_map<uint32_t,uint32_t> values;
 uint32_t AllocateId(){return next++;}
 void AddFunction(std::initializer_list<uint32_t> words) {
  const auto p=words.begin();const auto op=p[0],id=p[2];
  if(op==OpExtInst){if(p[4]!=GlslFindILsb)throw std::runtime_error("Unexpected GLSL op");const auto n=values.at(p[5]);values[id]=n?std::countr_zero(n):UINT32_MAX;return;}
  const auto a=values.at(p[3]);
  if(op==OpBitcast){values[id]=a;return;}
  const auto b=values.at(p[4]);
  if(op==OpIAdd)values[id]=a+b;
  else if(op==OpINotEqual)values[id]=a!=b;
  else if(op==OpSelect)values[id]=a?b:values.at(p[5]);
  else throw std::runtime_error("Unexpected SPIR-V op");
 }
};
struct EmitterState {Builder builder;uint32_t uint_type=1,int_type=2,bool_type=3,glsl_std450=4,result=0,reads=0,writes=0;};
uint32_t ConstantU32(EmitterState* s,uint32_t n){const auto id=s->builder.AllocateId();s->builder.values[id]=n;return id;}
uint32_t EmitSequentialValueLoad(EmitterState* s,const IR::Operand& o,uint32_t half){if(s->writes)throw std::runtime_error("Overlapping source read after destination write");++s->reads;return ConstantU32(s,static_cast<uint32_t>(o.value>>(32*half)));}
void EmitStoreU32(EmitterState* s,const IR::Operand&,uint32_t id){++s->writes;s->result=s->builder.values.at(id);}
