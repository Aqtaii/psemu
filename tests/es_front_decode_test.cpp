#include "graphics/shader/recompiler/ShaderDecoder.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace Libs::Graphics::ShaderRecompiler::Decoder;
void Require(bool value) { if (!value) throw std::runtime_error("ES decoder regression"); }
int main() {
 // Captured ES executable prefix, followed by inline non-instruction data.
 std::array<uint32_t,18> code {0x8f6a9003u,0x94fe6ac1u,0xbf88000bu,0xd7650005u,
  0x000100c1u,0x93eaff03u,0x00040018u,0xd7660005u,0x00020ac1u,0xd7460005u,
  0x4150c6au,0x340a0a82u,0xd8340000u,0x00000805u,0xbf8cc07fu,0xbe802006u,
  0xffffffffu,0xffffffffu};
 Program front; std::string error;
 Require(DecodeFrontProgram(code,&front,&error));
 Require(front.code.size()==16 && front.instructions.size()==11);
 Require(front.instructions.back().opcode==Opcode::SSetpcB64);
 Require(front.instructions.back().pc==60 && front.instructions.back().src0.reg==6);
 Program ordinary;
 Require(!DecodeProgram(code,&ordinary,&error)); // Normal stages still reject the data tail.
 auto bad=code; bad[15]=0xbe802004u; Require(!DecodeFrontProgram(bad,&front,&error));
 bad=code; bad[15]=0xbf810000u; Require(!DecodeFrontProgram(bad,&front,&error));
 Require(!DecodeFrontProgram(std::span(code).first(15),&front,&error));
 bad=code; bad[2]=0xbf88000du; Require(!DecodeFrontProgram(bad,&front,&error)); // Branch into data.
 bad=code; bad[2]=0xbf880000u; Require(DecodeFrontProgram(bad,&front,&error)); // Valid next instruction.
 bad=code; bad[2]=0xbf880001u; Require(!DecodeFrontProgram(bad,&front,&error)); // Middle of a 2-DWORD op.
 // Fused ES/GS keeps one register/LDS lifetime and relocates GS branches.
 std::vector<uint32_t> joined; Program fused;
 std::array<uint32_t,3> back {0xbf820001u,0xbf800000u,0xbf810000u};
 Require(DecodeFusedProgram(code,back,&joined,&fused,&error));
 Require(joined.size()==19 && fused.code.data()==joined.data());
 Require(fused.instructions[10].opcode==Opcode::SBranch && fused.instructions[10].branch_target==64);
 Require(fused.instructions[11].pc==64 && fused.instructions[11].branch_target==72);
 Require(fused.instructions.back().pc==72);
 back[0]=0xbf82ffffu; Require(DecodeFusedProgram(code,back,&joined,&fused,&error)); // GS-local loop.
 Require(fused.instructions[11].branch_target==64);
 back[0]=0xbf82fffeu; Require(!DecodeFusedProgram(code,back,&joined,&fused,&error)); // Cannot jump into ES.
 back[0]=0xbe801f00u; Require(!DecodeFusedProgram(code,back,&joined,&fused,&error)); // Needs GS guest base.
 std::array<uint32_t,4> middle {0xbf820001u,0xd7650005u,0x000100c1u,0xbf810000u};
 Require(!DecodeFusedProgram(code,middle,&joined,&fused,&error));
 Require(!DecodeFusedProgram(code,{},&joined,&fused,&error));
 std::puts("Captured ES handoff, inline data, branch boundaries and ordinary decoder passed");
}
