#pragma once
#include "graphics/shader/recompiler/ShaderIR.h"
#include <array>
#include <map>
#include <utility>
namespace Libs::Graphics::ShaderRecompiler::Spirv {
// BUFFER_LOAD_DWORDXn reads all address operands before any destination write.
// Values here are SPIR-V IDs, captured once for the original guest instruction.
template<class Load>
std::array<uint32_t,4> CaptureBufferAddressGroup(const IR::Instruction& inst, Load load) {
 std::array<uint32_t,4> values {};
 for(uint32_t i=0; i<inst.src_count && i<values.size(); i++) values[i]=load(inst.src[i]);
 return values;
}
// Each virtual half captures its address before overlapping destination writes.
class BufferAddressGroups {
public:
 template<class Load>
 void Capture(const IR::Instruction& inst, uint32_t half, Load load) {
  values[{inst.pc,half}]=CaptureBufferAddressGroup(inst,load);
 }
 const std::array<uint32_t,4>* Find(uint32_t pc,uint32_t half) const {
  const auto found=values.find({pc,half});
  return found==values.end() ? nullptr : &found->second;
 }
private:
 std::map<std::pair<uint32_t,uint32_t>,std::array<uint32_t,4>> values;
};
}
