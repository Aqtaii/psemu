#pragma once
#include <cstdint>
namespace Libs::Graphics::ShaderRecompiler::IR {
// Binding payload sizes are validated by AllocateBindings; SPIR-V stores vec4 arrays.
constexpr uint32_t PushConstantBlockExtent(uint32_t payload_bytes) {
 return (payload_bytes+15u)&~15u;
}
}
