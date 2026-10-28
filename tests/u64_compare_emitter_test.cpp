#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
enum {OpUGreaterThan=1,OpIEqual,OpINotEqual,OpLogicalAnd,OpLogicalOr,OpISub,OpSelect};
namespace IR {enum class Opcode {IAddCarryU32,ISubBorrowU32};struct Operand {uint64_t value=0;};struct Instruction {Operand src[3],dst,dst2;uint32_t src_count=2,pc=0;Opcode op=Opcode::IAddCarryU32;};struct BasicBlock {std::vector<Instruction> instructions;};}
struct Builder {
 uint32_t next=10;std::unordered_map<uint32_t,uint32_t> values;
 uint32_t AllocateId(){return next++;}
 void AddFunction(std::initializer_list<uint32_t> words) {
  auto p=words.begin();const auto opcode=p[0],id=p[2],a=values.at(p[3]),b=values.at(p[4]);
  switch(opcode) {
   case OpUGreaterThan:values[id]=a>b;break;
   case OpIEqual:values[id]=a==b;break;
   case OpINotEqual:values[id]=a!=b;break;
   case OpLogicalAnd:values[id]=a&&b;break;
   case OpLogicalOr:values[id]=a||b;break;
   case OpISub:values[id]=a-b;break;
   case OpSelect:values[id]=a?b:values.at(p[5]);break;
   default:throw std::runtime_error("Unexpected SPIR-V operation");
  }
 }
};
struct EmitterState {Builder builder;uint32_t bool_type=1;bool result=false;uint32_t uint_type=2,lane=0,subtraction=0;bool borrow=false;};
uint32_t EmitSequentialValueLoad(EmitterState* s,const IR::Operand& op,uint32_t dword) {
 const auto id=s->builder.AllocateId();s->builder.values[id]=static_cast<uint32_t>(op.value>>(32*dword));return id;
}
void EmitCompareResult(EmitterState* s,const IR::Operand&,uint32_t condition){s->result=s->builder.values.at(condition)!=0;}
uint32_t ConstantU32(EmitterState* s,uint32_t value){const auto id=s->builder.AllocateId();s->builder.values[id]=value;return id;}
uint32_t EmitValueLoad(EmitterState* s,const IR::Operand& op){return ConstantU32(s,static_cast<uint32_t>(op.value));}
uint32_t EmitLaneMaskOperandActiveBool(EmitterState* s,const IR::Operand& op){return ConstantU32(s,(op.value>>s->lane)&1);}
void EmitStoreU32(EmitterState* s,const IR::Operand&,uint32_t value){s->subtraction=s->builder.values.at(value);}
void EmitLaneMaskPairFromBool(EmitterState* s,const IR::Operand&,uint32_t value){s->borrow=s->builder.values.at(value)!=0;}
namespace Decoder {
enum class Opcode {VAddcU32,VSubCoCiU32,VSubrevCoCiU32};
struct Instruction {Opcode opcode;uint32_t pc=0;IR::Operand dst,dst2,src0,src1,src2;};
}
using IR::Instruction;using IR::BasicBlock;using IR::Opcode;
bool LowerRegisterOperand(const IR::Operand& a,IR::Operand* b,std::string*){*b=a;return true;}
bool LowerSourceOperand(const IR::Operand& a,IR::Operand* b,std::string*){*b=a;return true;}
