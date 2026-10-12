#include "graphics/shader/recompiler/spirvEmitter/bufferMemoryGroup.h"
#include <array>
#include <stdexcept>
#include <cstdio>
using namespace Libs::Graphics::ShaderRecompiler;
void Require(bool ok) {if(!ok) throw std::runtime_error("buffer address overlap regression");}
int main() {
 IR::Instruction load; load.op=IR::Opcode::BufferLoadDword; load.pc=0x90;
 load.memory.offen=true; load.memory.component_count=2; load.memory.offset=16;
 load.src_count=2; load.src[0].kind=IR::OperandKind::Register;
 load.src[0].reg={IR::RegisterFile::Vector,4};
 load.src[1].kind=IR::OperandKind::ImmediateU32; load.src[1].imm=0;
 std::array<uint32_t,16> regs {}; regs[4]=24;
 unsigned reads=0;
 auto read=[&](const IR::Operand& op) {reads++;return op.kind==IR::OperandKind::Register?regs[op.reg.index]:op.imm;};
 auto captured=Spirv::CaptureBufferAddressGroup(load,read);
 // Captured VS: BUFFER_LOAD_DWORDX2 v[4:5],v4. First UV write overwrites its own address.
 const auto first_address=captured[0]+load.memory.offset+captured[1];
 regs[4]=0x40000000u;
 const auto second_address=captured[0]+load.memory.offset+4+captured[1];
 Require(first_address==40 && second_address==44 && reads==2);
 Require(regs[4]+load.memory.offset+4!=second_address); // Previous emitter reloaded corrupted v4.
 regs[4]=48;auto next=Spirv::CaptureBufferAddressGroup(load,read);
 Require(next[0]+load.memory.offset+4==68 && reads==4); // Next dynamic invocation re-captures.
 // idxen+offen capture both vector operands, plus scalar dynamic soffset.
 load.src_count=3;load.src[1]=load.src[0];load.src[1].reg.index=5;
 load.src[2]=load.src[0];load.src[2].reg.file=IR::RegisterFile::Scalar;load.src[2].reg.index=6;
 regs[4]=2;regs[5]=8;regs[6]=12;auto all=Spirv::CaptureBufferAddressGroup(load,read);
 regs[4]=99;regs[5]=100;regs[6]=101;
 Require(all[0]==2 && all[1]==8 && all[2]==12 && all[3]==0);
 load.src_count=4;load.src[3]=load.src[2];load.src[3].kind=IR::OperandKind::ImmediateU32;load.src[3].imm=7;
 Require(Spirv::CaptureBufferAddressGroup(load,read)[3]==7);
 // Same PC, two virtual halves: an upper capture must not replace the lower address.
 Spirv::BufferAddressGroups groups;
 load.src_count=2;load.src[1].kind=IR::OperandKind::ImmediateU32;load.src[1].imm=0;
 regs[4]=24;groups.Capture(load,0,read);
 regs[4]=64;groups.Capture(load,1,read);
 regs[4]=0x40000000; // Both address VGPRs can be overwritten by first load components.
 Require((*groups.Find(load.pc,0))[0]+load.memory.offset+4==44);
 Require((*groups.Find(load.pc,1))[0]+load.memory.offset+4==84);
 Require(groups.Find(load.pc,2)==nullptr && groups.Find(load.pc+4,0)==nullptr);
 regs[4]=48;groups.Capture(load,0,read); // Re-emission replaces only the selected half.
 Require((*groups.Find(load.pc,0))[0]+load.memory.offset+4==68);
 Require((*groups.Find(load.pc,1))[0]+load.memory.offset+4==84);
 std::puts("Captured UV overlap, fresh invocation and full address operand snapshot passed");
}
