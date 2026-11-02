#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_METADATACLEARRANGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_METADATACLEARRANGE_H_

#include "graphics/host_gpu/regionDefinitions.h"

namespace Libs::Graphics {

struct MetadataClearRange {
    uint64_t owner_address = 0;
    uint32_t first_layer = 0;
    uint32_t layer_count = 0;
    uint32_t clear_mask = 0;
};

// Virtual HTile clears may cover whole array layers, never partial metadata planes.
[[nodiscard]] inline bool PlanMetadataClearRange(
    uint64_t owner, uint64_t owner_size, uint32_t layers,
    uint64_t address, uint64_t size, MetadataClearRange* out) {
    constexpr uint64_t alignment = 32ull * 1024ull;
    if (out == nullptr || owner == 0 || owner >= TRACKER_ADDRESS_SIZE ||
        owner_size == 0 || owner_size > TRACKER_ADDRESS_SIZE - owner ||
        owner % alignment != 0 || layers == 0 || layers > 32 ||
        owner_size % layers != 0 || size == 0 || address < owner) {
        return false;
    }
    const auto plane = owner_size / layers;
    const auto offset = address - owner;
    if (plane % alignment != 0 || offset >= owner_size ||
        size > owner_size - offset || offset % plane != 0 || size % plane != 0) {
        return false;
    }
    const auto first = static_cast<uint32_t>(offset / plane);
    const auto count = static_cast<uint32_t>(size / plane);
    *out = {owner, first, count,
            static_cast<uint32_t>(((uint64_t{1} << count) - 1) << first)};
    return true;
}

} // namespace Libs::Graphics
#endif
