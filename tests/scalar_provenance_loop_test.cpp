#include "graphics/shader/recompiler/ScalarProvenance.h"
#include <cstdio>
using namespace Libs::Graphics::ShaderRecompiler::IR;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
 Program p; p.user_data_count=1; p.wave_size=32; p.blocks.resize(7);
 const std::vector<std::vector<uint32_t>> successors{{1,3},{2,3},{3},{4,0},{5,1},{6,0},{4}};
 for (uint32_t i=0;i<p.blocks.size();++i) {
  auto& block=p.blocks[i]; block.id=i; block.start_pc=i*4; block.successors=successors[i];
  for (auto j: successors[i]) p.blocks[j].predecessors.push_back(i);
 }
 Instruction write; write.pc=8; write.op=Opcode::MoveU32;
 write.dst.kind=OperandKind::Register; write.dst.reg.file=RegisterFile::Scalar; write.dst.reg.index=0;
 write.src_count=1; write.src[0].kind=OperandKind::ImmediateU32; write.src[0].imm=0x55;
 p.blocks[2].instructions.push_back(write);
 std::string error; CHECK(BuildScalarProvenance(&p,&error)); CHECK(error.empty());
 auto id=p.blocks[2].instructions[0].scalar_value;
 CHECK(id<p.provenance.values.size()); CHECK(p.provenance.values[id].op==ScalarValueOp::Constant);
 CHECK(p.provenance.values[id].imm==0x55); CHECK(p.provenance.values.size()<128);
 bool phi=false; for(const auto& value:p.provenance.values) if(value.op==ScalarValueOp::Phi) {phi=true; CHECK(!value.phi_args.empty());}
 CHECK(phi); const auto count=p.provenance.values.size(); CHECK(BuildScalarProvenance(&p,&error)); CHECK(p.provenance.values.size()==count);
 Program linear;linear.user_data_count=1;linear.blocks.resize(2);linear.blocks[0].successors={1};linear.blocks[1].predecessors={0};linear.blocks[0].instructions={write};
 CHECK(BuildScalarProvenance(&linear,&error));
 for(const auto& value:linear.provenance.values) CHECK(value.op!=ScalarValueOp::Phi);
 std::puts("PASS: cyclic scalar provenance converges, preserves constants and rebuilds deterministically");
}
