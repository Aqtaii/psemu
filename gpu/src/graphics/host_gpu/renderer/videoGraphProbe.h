#pragma once
#include <cstdint>
namespace Libs::Graphics {
// Metadata-only trace window, opened by the opt-in completed-video draw probe.
bool VideoGraphProbeActive();
// Opt-in watch until the last video-produced allocation is retired; independent edge budget.
struct VulkanImage;
void VideoGraphProbeImageDestroyed(uint64_t allocation_id);
bool VideoGraphProbeMatchesAllocation(uint64_t allocation_id);
bool VideoGraphProbeTouchesTarget(uint64_t address, uint64_t size);
void VideoGraphProbeLogTransfer(const char* operation, const VulkanImage* source_image,
                               const VulkanImage* target_image, uint64_t source_address,
                               uint64_t target_address, uint64_t size);
}
