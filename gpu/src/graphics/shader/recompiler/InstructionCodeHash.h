#pragma once
#include <cstdint>
#include <span>

namespace Libs::Graphics::ShaderRecompiler {
// Stable instruction identity; independent of guest code address and trailing data.
[[nodiscard]] inline uint64_t InstructionCodeHash(std::span<const uint32_t> words) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word: words) {
        for (uint32_t byte = 0; byte < 4; ++byte) {
            hash ^= (word >> (byte * 8)) & 0xffu;
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}
} // namespace Libs::Graphics::ShaderRecompiler
