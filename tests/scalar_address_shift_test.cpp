#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "graphics/shader/recompiler/SrtWalker.h"
#include <cstdio>
using namespace Libs::Graphics::ShaderRecompiler::IR;
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
Operand reg(uint32_t n) {Operand o;o.kind=OperandKind::Register;o.reg.file=RegisterFile::Scalar;o.reg.index=n;return o;}
Operand imm(uint32_t n) {Operand o;o.kind=OperandKind::ImmediateU32;o.imm=n;return o;}
Instruction move(uint32_t dst,Operand src) {Instruction i;i.op=Opcode::MoveU32;i.dst=reg(dst);i.src_count=1;i.src[0]=src;return i;}
int main() {
 for(bool left:{false,true}) for(uint32_t amount=0;amount<128;++amount)
 for(uint64_t input:{0ull,1ull,0x80000000ull,0x123456789abcdef0ull,0xffffffffffffffffull})
 for(uint32_t dst:{8u,28u,29u}) {
  Program p;p.user_data_count=0;p.blocks.resize(1);
  p.blocks[0].instructions={move(28,imm(uint32_t(input))),move(29,imm(uint32_t(input>>32)))};
  Instruction shift;shift.pc=0x2090;shift.op=left?Opcode::ShiftLeftLogicalU64:Opcode::ShiftRightLogicalU64;
  shift.dst=reg(dst);shift.src_count=2;shift.src[0]=reg(28);shift.src[1]=imm(amount);
  p.blocks[0].instructions.push_back(shift);
  p.blocks[0].instructions.push_back(move(40,reg(dst)));
  p.blocks[0].instructions.push_back(move(41,reg(dst+1)));
  std::string error;CHECK(BuildScalarProvenance(&p,&error));
  uint32_t low=0,high=0;
  CHECK(FoldScalarConstant(p.provenance,p.blocks[0].instructions[3].scalar_value,&low));
  CHECK(FoldScalarConstant(p.provenance,p.blocks[0].instructions[4].scalar_value,&high));
  const uint64_t expected=left?input<<(amount&63):input>>(amount&63);
  CHECK((uint64_t(low)|(uint64_t(high)<<32))==expected);
 }
 Program dynamic;dynamic.user_data_count=0;dynamic.blocks.resize(1);
 Instruction shift;shift.op=Opcode::ShiftLeftLogicalU64;shift.dst=reg(8);shift.src_count=2;
 shift.src[0]=reg(28);shift.src[1]=imm(3);dynamic.blocks[0].instructions={shift,move(40,reg(8)),move(41,reg(9))};
 std::string error;CHECK(BuildScalarProvenance(&dynamic,&error));uint32_t result=0;
 CHECK(!FoldScalarConstant(dynamic.provenance,dynamic.blocks[0].instructions[1].scalar_value,&result));
 CHECK(!FoldScalarConstant(dynamic.provenance,dynamic.blocks[0].instructions[2].scalar_value,&result));
#ifdef _WIN32
 Program read;read.srt_plan_complete=true;read.provenance.values.resize(4);
 read.provenance.values[0].op=ScalarValueOp::Constant;read.provenance.values[0].imm=0x24;
 read.provenance.values[1].op=ScalarValueOp::Constant;
 read.provenance.values[2].op=ScalarValueOp::Constant;
 read.provenance.values[3].op=ScalarValueOp::ReadConst;
 read.provenance.values[3].args={0,1,2};read.provenance.values[3].pc=0x2150;
 read.srt.reads.push_back({3,0,0x2150});
 std::vector<uint32_t> flat{0xfeedface};SrtRuntime runtime;
 CHECK(!WalkSrt(read,runtime,&flat,&error));
 CHECK(flat==std::vector<uint32_t>{0xfeedface});
 CHECK(error.find("0000000000000024")!=std::string::npos);
 uint32_t word=0x12345678;auto address=reinterpret_cast<uint64_t>(&word);
 read.provenance.values[0].imm=uint32_t(address);read.provenance.values[1].imm=uint32_t(address>>32);
 CHECK(WalkSrt(read,runtime,&flat,&error));CHECK(flat==std::vector<uint32_t>{word});
#endif
 std::puts("PASS: scalar address pair shifts, cross-word carries, overlapping registers, masked counts and unknown sources");
}
