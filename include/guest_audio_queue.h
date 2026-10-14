#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
// Layout and defaults from Kyty AudioOut2ContextParam; no PCM backend is modeled here.
struct GuestAudioContextParam {
    uint32_t max_ports=256, max_object_ports=256, guarantee_object_ports=0;
    uint32_t queue_depth=4, num_grains=512, flags=1;
    std::array<uint32_t,10> reserved{};
};
static_assert(sizeof(GuestAudioContextParam)==64);
struct GuestAudioQueue {
    uint32_t depth=4, queued=0;
    uint64_t grain_micros=10666, last_update=0;
    GuestAudioQueue(uint32_t capacity=4,uint32_t grains=512)
        : depth(capacity ? capacity : 4),
          grain_micros((std::max)(uint64_t(1000),uint64_t(grains ? grains : 512)*1000000/48000)) {}
    void Advance(uint64_t now) {
        if (!queued) {last_update=now;return;}
        if (now<=last_update) return;
        const auto drained=(std::min)(uint64_t(queued),(now-last_update)/grain_micros);
        queued-=static_cast<uint32_t>(drained);
        last_update+=drained*grain_micros;
    }
    bool Push(uint64_t now) {
        Advance(now);
        if (queued>=depth) return false;
        if (!queued) last_update=now;
        ++queued;return true;
    }
    uint32_t Available() const {return depth-queued;}
};
