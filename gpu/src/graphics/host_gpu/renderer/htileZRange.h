#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>
#include <utility>

namespace Libs::Graphics {

// This utility reader extracts only ZMin/ZMax; the snapshot is never guest HTile backing.
constexpr uint64_t HtileZRangeReaderHash = 0x5fd60ef3de600325ull;
constexpr uint32_t HtileZRangeReaderWords = 63;
constexpr uint32_t HtileExpandedFullRange = 0xfffc000fu;
constexpr uint64_t MaxHtileZRangeSnapshotBytes = 16ull * 1024ull * 1024ull;

struct HtileZRangeLayout {
    uint32_t width = 0, height = 0, pitch_blocks = 0;
    uint32_t tiles_x = 0, tiles_y = 0;
    uint64_t metadata_bytes = 0, view_bytes = 0;
};

[[nodiscard]] inline bool MakeHtileZRangeLayout(
    uint32_t width, uint32_t height, uint32_t pitch_blocks,
    uint64_t metadata_bytes, uint64_t view_bytes, HtileZRangeLayout* out) {
    if (!out || !width || !height || width > 8192 || height > 8192 ||
        width % 8 || height % 8 || pitch_blocks != (width + 1023u) / 1024u ||
        view_bytes < metadata_bytes || view_bytes > MaxHtileZRangeSnapshotBytes ||
        view_bytes % 4 != 0) {
        return false;
    }
    const auto tiles_y = height / 8u;
    const uint64_t expected = uint64_t(pitch_blocks) * ((tiles_y + 63u) / 64u) * 32768u;
    if (!expected || metadata_bytes != expected) {
        return false;
    }
    *out = {width, height, pitch_blocks, width / 8u, tiles_y, metadata_bytes, view_bytes};
    return true;
}

// Address equation used by the captured GFX10 Z-only metadata reader.
// One macro tile contains 128x64 eight-pixel tiles, in 32 KiB.
[[nodiscard]] inline uint64_t HtileZRangeByteOffset(
    const HtileZRangeLayout& layout, uint32_t x, uint32_t y) {
    if (x >= layout.pitch_blocks * 128u || y >= ((layout.tiles_y + 63u) / 64u) * 64u) {
        return UINT64_MAX;
    }
    const uint32_t low = ((x << 1) & 0x10u) | ((x << 2) & 0x44u) |
        ((x << 8) & 0x4300u) | ((x << 7) & 0x1400u) | ((x << 9) & 0x800u) |
        ((y << 2) & 0x28u) | ((y << 3) & 0x80u);
    const uint64_t high = (uint64_t(x >> 7) + uint64_t(layout.pitch_blocks) * (y >> 6)) << 15;
    return high + (low ^ ((y << 8) & 0x2f00u));
}

// Expand actual native depth into conservative 14-bit bounds. Padding retains
// PAL's valid expanded [0,1] value. It is not published as physical metadata.
[[nodiscard]] inline bool BuildHtileZRangeSnapshot(
    std::span<const float> depth, const HtileZRangeLayout& layout,
    std::vector<uint32_t>* out) {
    HtileZRangeLayout checked;
    if (!out || !MakeHtileZRangeLayout(layout.width, layout.height, layout.pitch_blocks,
            layout.metadata_bytes, layout.view_bytes, &checked) ||
        layout.tiles_x != checked.tiles_x || layout.tiles_y != checked.tiles_y ||
        depth.size() != uint64_t(layout.width) * layout.height) {
        return false;
    }
    std::vector<uint32_t> result(static_cast<size_t>(layout.view_bytes / 4u),
                                 HtileExpandedFullRange);
    for (uint32_t y = 0; y < layout.tiles_y; ++y) {
        for (uint32_t x = 0; x < layout.tiles_x; ++x) {
            float low = 1.f, high = 0.f;
            for (uint32_t dy = 0; dy < 8; ++dy) {
                for (uint32_t dx = 0; dx < 8; ++dx) {
                    const float value = depth[size_t(y * 8u + dy) * layout.width + x * 8u + dx];
                    if (!std::isfinite(value) || value < 0.f || value > 1.f) {
                        return false;
                    }
                    low = (std::min)(low, value);
                    high = (std::max)(high, value);
                }
            }
            const auto address = HtileZRangeByteOffset(layout, x, y);
            if (address >= layout.metadata_bytes || layout.metadata_bytes - address < 4) {
                return false;
            }
            const auto zmin = static_cast<uint32_t>(std::floor(double(low) * 16383.0));
            const auto zmax = static_cast<uint32_t>(std::ceil(double(high) * 16383.0));
            result[static_cast<size_t>(address / 4)] = (zmax << 18) | (zmin << 4) | 0xfu;
        }
    }
    *out = std::move(result);
    return true;
}
} // namespace Libs::Graphics
