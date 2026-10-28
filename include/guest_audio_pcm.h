#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
// AudioOut2 interleaved float/s16: mono, stereo, or 7.1 bed. Object ports are
// separate.
inline bool MixGuestAudioStereo(std::span<const unsigned char> pcm,
                                uint32_t format, size_t frames,
                                std::span<float> mix, uint64_t *nonfinite,
                                float *source_peak = nullptr) {
  const uint32_t encoded = (format >> 8) & 255,
                 channels = encoded ? encoded : 2, type = format & 0x7f;
  if ((channels != 1 && channels != 2 && channels != 8) || type > 1 ||
      frames > 8192 || mix.size() != frames * 2 ||
      pcm.size() != frames * channels * (type == 0 ? 4 : 2))
    return false;
  const auto sample = [&](size_t index) {
    float v;
    if (type == 0)
      std::memcpy(&v, pcm.data() + index * 4, 4);
    else {
      int16_t s;
      std::memcpy(&s, pcm.data() + index * 2, 2);
      v = float(s) / 32768.0f;
    }
    if (!std::isfinite(v)) {
      ++*nonfinite;
      return 0.0f;
    }
    if (source_peak) *source_peak = std::max(*source_peak, std::abs(v));
    return v;
  };
  for (size_t f = 0; f < frames; f++) {
    std::array<float, 8> values{};
    for (size_t c = 0; c < channels; c++)
      values[c] = sample(f * channels + c);
    if (channels == 8) {
      // FL FR FC LFE plus left/right surrounds. Both standard rear/side orders
      // have the same coefficients. Omit LFE; normalize the sum to retain
      // headroom.
      constexpr float center = 0.70710678f,
                      normalization = 1.0f / (2.0f + center);
      for (size_t c = 0; c < 2; c++)
        mix[f * 2 + c] += (values[c] + center * values[2] +
                           0.5f * (values[4 + c] + values[6 + c])) *
                          normalization;
    } else
      for (size_t c = 0; c < 2; c++)
        mix[f * 2 + c] += values[channels == 1 ? 0 : c];
  }
  return true;
}