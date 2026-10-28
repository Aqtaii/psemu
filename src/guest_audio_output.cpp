#include "guest_audio_output.h"
#include "guest_audio_pcm.h"
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <windows.h>
#include <mmsystem.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
struct PortParam {
  uint16_t type, pad;
  uint32_t format, frequency, flags;
  uint64_t user;
  uint32_t reserved[10];
};
struct Attribute {
  uint32_t id;
  int32_t reserved;
  const void *value;
  uint64_t size;
};
struct PortState {
  uint16_t output = 1;
  uint8_t channels = 2, pad = 0;
  int16_t volume = 127;
  uint16_t reroutes = 0;
  uint32_t flags = 0, pad2 = 0;
  uint64_t reserved[6]{};
};
static_assert(sizeof(PortParam) == 64 && sizeof(Attribute) == 24 &&
              sizeof(PortState) == 64);
struct Port {
  uint64_t context;
  PortParam param;
  const void *pcm = nullptr;
};
struct Slot {
  WAVEHDR header{};
  std::vector<int16_t> samples;
};
struct Context {
  uint32_t grains = 512;
  uint64_t pushes = 0, nonzero = 0, drops = 0, input_nonzero = 0;
  HWAVEOUT device = nullptr;
  bool open_attempted = false;
  std::array<Slot, 8> slots;
  ~Context() {
    if (device) {
      waveOutReset(device);
      for (auto &s : slots)
        if (s.header.dwFlags & WHDR_PREPARED)
          waveOutUnprepareHeader(device, &s.header, sizeof(s.header));
      waveOutClose(device);
    }
  }
  void Submit(const std::vector<float> &mix) {
    if (!open_attempted) {
      open_attempted = true;
      WAVEFORMATEX fmt{};
      fmt.wFormatTag = WAVE_FORMAT_PCM;
      fmt.nChannels = 2;
      fmt.nSamplesPerSec = 48000;
      fmt.wBitsPerSample = 16;
      fmt.nBlockAlign = 4;
      fmt.nAvgBytesPerSec = 192000;
      const auto status =
          waveOutOpen(&device, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
      std::printf("[AUDIO-HOST] waveOutOpen=%u rate=48000 channels=2\n",
                  status);
      if (status != MMSYSERR_NOERROR)
        device = nullptr;
    }
    if (!device)
      return;
    for (auto &s : slots) {
      if (s.header.dwFlags & WHDR_PREPARED) {
        if (!(s.header.dwFlags & WHDR_DONE))
          continue;
        if (waveOutUnprepareHeader(device, &s.header, sizeof(s.header)) !=
            MMSYSERR_NOERROR)
          continue;
      }
      s.samples.resize(mix.size());
      for (size_t i = 0; i < mix.size(); i++)
        s.samples[i] =
            static_cast<int16_t>(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f);
      s.header = {};
      s.header.lpData = reinterpret_cast<char *>(s.samples.data());
      s.header.dwBufferLength = static_cast<DWORD>(s.samples.size() * 2);
      const auto prepared =
          waveOutPrepareHeader(device, &s.header, sizeof(s.header));
      const auto written =
          prepared == MMSYSERR_NOERROR
              ? waveOutWrite(device, &s.header, sizeof(s.header))
              : prepared;
      if (written != MMSYSERR_NOERROR) {
        if (s.header.dwFlags & WHDR_PREPARED)
          waveOutUnprepareHeader(device, &s.header, sizeof(s.header));
        s.header = {};
        if (drops++ < 4)
          std::printf("[AUDIO-HOST] submit_error=%u\n", written);
      }
      return;
    }
    ++drops;
  }
};
std::mutex mutex_;
std::unordered_map<uint64_t, std::unique_ptr<Context>> contexts;
std::unordered_map<uint64_t, Port> ports;
uint64_t next_port = 1, next_user = 1;
uint32_t next_acm = 1;
constexpr int64_t bad_address = -2147352562LL, invalid = -2147352554LL;
uint8_t Channels(uint32_t format) {
  const auto c = (format >> 8) & 255;
  return static_cast<uint8_t>(c ? c : 2);
}
} // namespace
void GuestAudioRegisterContext(uint64_t context, uint32_t grains) {
  std::lock_guard lock(mutex_);
  auto state = std::make_unique<Context>();
  state->grains = grains ? grains : 512;
  contexts[context] = std::move(state);
}
void GuestAudioDestroyContext(uint64_t context) {
  std::lock_guard lock(mutex_);
  std::erase_if(ports,
                [&](const auto &p) { return p.second.context == context; });
  contexts.erase(context);
}
bool GuestAudioOutputCall(std::string_view name, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t *result,
                          GuestAudioRead readable, GuestAudioWrite writable) {
  auto done = [&](int64_t value) {
    *result = static_cast<uint64_t>(value);
    return true;
  };
  if (name == "sceAcmContextCreate") {
    auto *out = reinterpret_cast<uint32_t *>(a0);
    if (!writable(out, 4))
      return done(bad_address);
    std::lock_guard lock(mutex_);
    *out = next_acm++;
    std::printf("[AUDIO-ACM] created_context=%u (32-bit output)\n", *out);
    return done(0);
  }
  if (name == "sceAudioOut2UserCreate") {
    auto *out = reinterpret_cast<uint64_t *>(a1);
    if (!writable(out, 8))
      return done(bad_address);
    std::lock_guard lock(mutex_);
    *out = next_user++;
    return done(0);
  }
  if (name == "sceAudioOut2PortCreate") {
    const auto *p = reinterpret_cast<const PortParam *>(a1);
    auto *out = reinterpret_cast<uint64_t *>(a2);
    if (!readable(p, sizeof(*p)) || !writable(out, 8))
      return done(bad_address);
    PortParam param;
    std::memcpy(&param, p, sizeof(param));
    std::lock_guard lock(mutex_);
    if (!contexts.contains(a0))
      return done(invalid);
    if (ports.size() >= 256)
      return done(invalid);
    const auto id = next_port++;
    ports.emplace(id, Port{a0, param});
    *out = id;
    std::printf("[AUDIO-PORT] context=%llu port=%llu type=0x%x format=0x%x "
                "channels=%u rate=%u flags=0x%x grains=%u\n",
                a0, id, param.type, param.format, Channels(param.format),
                param.frequency, param.flags, contexts[a0]->grains);
    std::fflush(stdout);
    return done(0);
  }
  if (name == "sceAudioOut2PortDestroy") {
    std::lock_guard lock(mutex_);
    return done(ports.erase(a0) ? 0 : invalid);
  }
  if (name == "sceAudioOut2PortGetState") {
    auto *out = reinterpret_cast<PortState *>(a1);
    if (!writable(out, sizeof(*out)))
      return done(bad_address);
    std::lock_guard lock(mutex_);
    const auto p = ports.find(a0);
    if (p == ports.end())
      return done(invalid);
    PortState state;
    state.channels = Channels(p->second.param.format);
    std::memcpy(out, &state, sizeof(state));
    return done(0);
  }
  if (name == "sceAudioOut2GetSpeakerInfo") {
    auto *out = reinterpret_cast<unsigned char *>(a0);
    if (!writable(out, 80))
      return done(bad_address);
    std::array<unsigned char, 80> info{};
    uint32_t bits = 3;
    std::memcpy(info.data() + 4, &bits, 4);
    const int16_t angles[4] = {-30, 0, 30, 0};
    std::memcpy(info.data() + 16, angles, 8);
    std::memcpy(out, info.data(), 80);
    return done(0);
  }
  if (name == "sceAudioOut2PortSetAttributes") {
    const auto *attrs = reinterpret_cast<const Attribute *>(a1);
    if (a2 > 256)
      return done(invalid);
    if (a2 && !readable(attrs, size_t(a2) * sizeof(Attribute)))
      return done(bad_address);
    const void *pcm = nullptr;
    bool has_pcm = false;
    for (size_t i = 0; i < a2; i++) {
      Attribute attr;
      std::memcpy(&attr, attrs + i, sizeof(attr));
      if (attr.id == 0) {
        if (attr.size < 8 || !readable(attr.value, 8))
          return done(bad_address);
        std::memcpy(&pcm, attr.value, 8);
        has_pcm = true;
      }
    }
    std::lock_guard lock(mutex_);
    const auto p = ports.find(a0);
    if (p == ports.end())
      return done(invalid);
    if (has_pcm)
      p->second.pcm = pcm;
    return done(0);
  }
  return false;
}
void GuestAudioPushPcm(uint64_t context, GuestAudioRead readable) {
  std::lock_guard lock(mutex_);
  const auto it = contexts.find(context);
  if (it == contexts.end())
    return;
  auto &state = *it->second;
  if (!state.grains || state.grains > 8192)
    return;
  std::vector<float> mix(size_t(state.grains) * 2, 0.0f);
  uint32_t active = 0, objects = 0, missing = 0, unsupported = 0,
           invalid_pcm = 0;
  float peak = 0, object_peak = 0, input_peak = 0;
  uint64_t nonfinite = 0;
  for (const auto &[id, p] : ports) {
    if (p.context != context)
      continue;
    if (!p.pcm) {
      ++missing;
      continue;
    }
    const auto channels = Channels(p.param.format);
    const auto type = p.param.format & 0x7f;
    // Object/haptic ports require separate spatial rendering; never play them
    // as speakers.
    if ((p.param.type & 255) == 10 ||
        (channels != 1 && channels != 2 && channels != 8) || type > 1 ||
        p.param.frequency != 48000) {
      ++unsupported;
      continue;
    }
    const auto bytes = size_t(state.grains) * channels * (type == 0 ? 4 : 2);
    if (!readable(p.pcm, bytes)) {
      ++invalid_pcm;
      continue;
    }
    std::vector<unsigned char> copy(bytes);
    std::memcpy(copy.data(), p.pcm, bytes);
    if ((p.param.type & 0xff00) == 0x100) {
      ++objects;
      std::vector<float> object_mix(mix.size(), 0.0f);
      MixGuestAudioStereo(copy, p.param.format, state.grains, object_mix,
                          &nonfinite);
      for (float v : object_mix)
        object_peak = std::max(object_peak, std::abs(v));
    } else {
      ++active;
      MixGuestAudioStereo(copy, p.param.format, state.grains, mix, &nonfinite, &input_peak);
    }
  }
  for (auto &v : mix) {
    if (!std::isfinite(v))
      v = 0;
    peak = std::max(peak, std::abs(v));
  }
  const bool first_nonzero = (peak > 0 && state.nonzero == 0) ||
                             (input_peak > 0 && state.input_nonzero == 0);
  if(input_peak > 0) ++state.input_nonzero;
  ++state.pushes;
  if (peak > 0) ++state.nonzero;
  // Open on the first valid bed buffer, including game-produced silence.
  // Host device failure must not remain hidden until a nonzero game sample.
  if (active > 0 || state.device) state.Submit(mix);
  if (state.pushes <= 8 || state.pushes % 1024 == 0 || first_nonzero) {
    std::printf("[AUDIO-PCM] context=%llu pushes=%llu active=%u objects=%u "
                "object_peak=%.8f missing=%u unsupported=%u invalid=%u "
                "nonfinite=%llu peak=%.8f input_peak=%.8f input_nonzero=%llu nonzero=%llu host_drops=%llu\n",
                context, state.pushes, active, objects, object_peak, missing,
                unsupported, invalid_pcm, nonfinite, peak, input_peak, state.input_nonzero, state.nonzero,
                state.drops);
    std::fflush(stdout);
  }
}