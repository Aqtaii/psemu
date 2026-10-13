#pragma once
#include <cstdint>
namespace Libs::Graphics {
struct MeshWave32Policy {bool supported;uint32_t required_size;};
// Prefer an explicit size whenever available: virtual guest64 requires native32.
constexpr MeshWave32Policy SelectMeshWave32Policy(bool enabled,bool stage_supported,
 uint32_t minimum,uint32_t maximum,uint32_t natural) {
 if(enabled && stage_supported && minimum<=32 && maximum>=32)return {true,32};
 return {natural==32,0};
}
}
