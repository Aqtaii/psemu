#pragma once
#include <cstdint>
namespace Libs::Graphics::ShaderRecompiler {
constexpr uint64_t AstroTransformNormalizeHash=0x25ea92806a8f33d1ull;
constexpr uint64_t AstroGsNormalizeHash=0xb42edebba57f9e3aull;
constexpr uint64_t AstroFusedGsNormalizeHash=0x1cd649e2963a321eull;
constexpr uint64_t AstroResizeNormalHash = 0x1e49bdc03a52ec35ull;
inline bool AstroNormalizationShader(uint64_t hash) {
 return hash==AstroTransformNormalizeHash || hash==AstroGsNormalizeHash || hash==AstroFusedGsNormalizeHash;
}
} // namespace Libs::Graphics::ShaderRecompiler
