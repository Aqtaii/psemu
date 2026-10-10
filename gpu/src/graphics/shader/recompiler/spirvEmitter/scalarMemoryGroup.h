#pragma once
#include "graphics/shader/recompiler/ShaderIR.h"
#include <array>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// Called before the first component writes any destination, including a
// component changed to LoadSrtDword by the scalar planning pass.
template<class Load>
std::array<uint32_t,3> CaptureScalarMemoryGroup(const IR::ScalarMemoryGroup& group, Load load) {
 IR::Operand low;
 low.kind=IR::OperandKind::Register;
 low.reg.file=IR::RegisterFile::Scalar;low.reg.index=group.base_sgpr;
 auto high=low;high.reg.index++;
 return {load(low),load(high),load(group.dynamic_offset)};
}
template<class Load>
std::array<uint32_t,5> CaptureScalarBufferGroup(const IR::ScalarMemoryGroup& group,Load load) {
 std::array<uint32_t,5> values;
 for(unsigned i=0;i<4;++i) {
  IR::Operand source;source.kind=IR::OperandKind::Register;
  source.reg={IR::RegisterFile::Scalar,group.base_sgpr+i};values[i]=load(source);
 }
 values[4]=load(group.dynamic_offset);return values;
}

}
