#pragma once
#include <cstdint>
namespace Libs::Graphics {
struct MeshWave32Policy {bool supported;uint32_t required_size;};
// A point ABI with a dispatcher is the validated virtual guest64 path.
// GS-root presence describes resources, not the physical subgroup width.
constexpr bool MeshProgramUsesHostWave32(uint32_t wave, bool native_masks,
 bool point_inputs, bool dispatcher) {
 return native_masks && (wave==32u || (wave==64u && point_inputs && dispatcher));
}

// Prefer an explicit size whenever available: virtual guest64 requires native32.
constexpr MeshWave32Policy SelectMeshWave32Policy(bool enabled,bool stage_supported,
 uint32_t minimum,uint32_t maximum,uint32_t natural) {
 if(enabled && stage_supported && minimum<=32 && maximum>=32)return {true,32};
 return {natural==32,0};
}
}
