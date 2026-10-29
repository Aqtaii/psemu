#pragma once
#include <cstdint>
namespace Libs::Graphics {
// Metadata-only trace window, opened by the opt-in completed-video draw probe.
bool VideoGraphProbeActive();
// Opt-in target watch for video draws64..128, independent of the general edge budget.
bool VideoGraphProbeTouchesTarget(uint64_t address, uint64_t size);
void VideoGraphProbeLogTransfer(const char* operation, const void* source_image,
                               const void* target_image, uint64_t source_address,
                               uint64_t target_address, uint64_t size);
}
