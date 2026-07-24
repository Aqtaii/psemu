#include "video.h"
#include "logger.h"
#include <sstream>
#include <mutex>

namespace Video {

namespace {
	uint64_t g_flip_count = 0;
	std::mutex g_mutex;
}

void Init(uint32_t width, uint32_t height) {
    LOG_INFO("[VIDEO] PSemuVideo is disabled (using Kyty window natively).");
}

void Close() {
}

void SetAttribute(uint32_t width, uint32_t height, uint32_t pitch_in_pixel,
                  uint64_t pixel_format, uint32_t tiling_mode) {
}

void RegisterBuffer(int index, void* addr) {
}

uint64_t GetFlipCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_flip_count;
}

void Flip(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_flip_count++;
}

} // namespace Video
