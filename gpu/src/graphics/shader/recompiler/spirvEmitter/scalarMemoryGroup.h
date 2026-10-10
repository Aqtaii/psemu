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
}
