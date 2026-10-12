#pragma once
#include "graphics/shader/recompiler/ShaderIR.h"
#include <array>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// BUFFER_LOAD_DWORDXn reads all address operands before any destination write.
// Values here are SPIR-V IDs, captured once for the original guest instruction.
template<class Load>
std::array<uint32_t,4> CaptureBufferAddressGroup(const IR::Instruction& inst, Load load) {
 std::array<uint32_t,4> values {};
 for(uint32_t i=0; i<inst.src_count && i<values.size(); i++) values[i]=load(inst.src[i]);
 return values;
}
}
