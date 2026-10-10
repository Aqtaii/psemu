#include "graphics/shader/recompiler/ShaderIR.h"
#include <stdexcept>
#include <cstdio>
using namespace Libs::Graphics::ShaderRecompiler;
bool GdsContract(const IR::Instruction&,std::string*);
static void Check(bool ok) {if(!ok)throw std::runtime_error("GDS IR contract regression");}
int main() {
 for(auto op:{IR::Opcode::DsAppend,IR::Opcode::DsConsume}) {
  for(uint32_t offset:{0u,4u,8u,0xc68u,0xffffu}) {
   IR::Instruction inst;inst.op=op;inst.src_count=1;
   inst.dst.kind=IR::OperandKind::Register;
   inst.memory.kind=IR::ResourceKind::Gds;inst.memory.offset=offset;
   std::string error;
#if EXPECT_OLD_GDS_REJECTION
   Check(GdsContract(inst,&error)==(offset==0));
#else
   Check(GdsContract(inst,&error));
#endif
   inst.src_count=2;Check(!GdsContract(inst,&error));inst.src_count=1;
   inst.memory.resource=1;Check(!GdsContract(inst,&error));inst.memory.resource=0;
   inst.dst.kind=IR::OperandKind::Null;Check(!GdsContract(inst,&error));
  }
 }
 std::puts("PASS: actual SPIR-V instruction contract accepts GDS counter offsets");
}
