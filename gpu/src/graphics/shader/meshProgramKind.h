#pragma once
#include "graphics/guest_gpu/gpu_defs.h"
namespace Libs::Graphics {
enum class MeshProgramKind {Unsupported, Standalone, Fused};
[[nodiscard]] inline constexpr MeshProgramKind SelectMeshProgramKind(uint8_t binary_type) {
 switch(static_cast<Prospero::ShaderBinaryType>(binary_type)) {
 case Prospero::ShaderBinaryType::kGs:return MeshProgramKind::Standalone;
 case Prospero::ShaderBinaryType::kGsFront:return MeshProgramKind::Fused;
 default:return MeshProgramKind::Unsupported;
 }
}
}
