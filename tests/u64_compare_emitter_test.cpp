#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
enum {OpUGreaterThan=1,OpIEqual,OpLogicalAnd,OpLogicalOr};
namespace IR {struct Operand {uint64_t value=0;};struct Instruction {Operand src[2],dst;};}
struct Builder {
 uint32_t next=10;std::unordered_map<uint32_t,uint32_t> values;
 uint32_t AllocateId(){return next++;}
 void AddFunction(std::initializer_list<uint32_t> words) {
  auto p=words.begin();const auto opcode=p[0],id=p[2],a=values.at(p[3]),b=values.at(p[4]);
  switch(opcode) {
   case OpUGreaterThan:values[id]=a>b;break;
   case OpIEqual:values[id]=a==b;break;
   case OpLogicalAnd:values[id]=a&&b;break;
   case OpLogicalOr:values[id]=a||b;break;
   default:throw std::runtime_error("Unexpected SPIR-V operation");
  }
 }
};
struct EmitterState {Builder builder;uint32_t bool_type=1;bool result=false;};
uint32_t EmitSequentialValueLoad(EmitterState* s,const IR::Operand& op,uint32_t dword) {
 const auto id=s->builder.AllocateId();s->builder.values[id]=static_cast<uint32_t>(op.value>>(32*dword));return id;
}
void EmitCompareResult(EmitterState* s,const IR::Operand&,uint32_t condition){s->result=s->builder.values.at(condition)!=0;}
