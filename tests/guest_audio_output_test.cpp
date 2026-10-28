#define NOMINMAX
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <windows.h>
#include <mmsystem.h>
#include <source_location>
#include <vector>

static void Require(bool ok,
                    std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::fprintf(stderr, "Audio regression line %u\n", at.line());
    std::exit(1);
  }
}
static unsigned opens = 0, writes = 0, unprepares = 0, resets = 0, closes = 0;
static std::vector<WAVEHDR *> pending;
static MMRESULT WINAPI MockOpen(LPHWAVEOUT out, UINT, LPCWAVEFORMATEX f,
                                DWORD_PTR, DWORD_PTR, DWORD) {
  Require(f->nChannels == 2 && f->nSamplesPerSec == 48000 &&
          f->wBitsPerSample == 16);
  ++opens;
  *out = reinterpret_cast<HWAVEOUT>(1);
  return 0;
}
static MMRESULT WINAPI MockPrepare(HWAVEOUT, LPWAVEHDR h, UINT) {
  h->dwFlags |= WHDR_PREPARED;
  return 0;
}
static MMRESULT WINAPI MockWrite(HWAVEOUT, LPWAVEHDR h, UINT) {
  Require(h->dwFlags & WHDR_PREPARED);
  h->dwFlags &= ~WHDR_DONE;
  pending.push_back(h);
  ++writes;
  return 0;
}
static MMRESULT WINAPI MockUnprepare(HWAVEOUT, LPWAVEHDR h, UINT) {
  Require(h->dwFlags & WHDR_DONE);
  h->dwFlags = 0;
  ++unprepares;
  return 0;
}
static MMRESULT WINAPI MockReset(HWAVEOUT) {
  for (auto *h : pending)
    h->dwFlags |= WHDR_DONE;
  ++resets;
  return 0;
}
static MMRESULT WINAPI MockClose(HWAVEOUT) {
  Require(resets > 0);
  ++closes;
  return 0;
}
#define waveOutOpen MockOpen
#define waveOutPrepareHeader MockPrepare
#define waveOutWrite MockWrite
#define waveOutUnprepareHeader MockUnprepare
#define waveOutReset MockReset
#define waveOutClose MockClose
#include "../src/guest_audio_output.cpp"
struct Range {
  uintptr_t address;
  size_t size;
};
static std::vector<Range> ranges;
static void Allow(void *p, size_t n) {
  ranges.push_back({reinterpret_cast<uintptr_t>(p), n});
}
static bool Read(const void *p, size_t n) {
  const auto a = reinterpret_cast<uintptr_t>(p);
  for (auto r : ranges)
    if (a >= r.address && a - r.address <= r.size &&
        n <= r.size - (a - r.address))
      return true;
  return false;
}
static bool Write(void *p, size_t n) { return Read(p, n); }
static uint64_t Call(const char *name, uint64_t a0, uint64_t a1 = 0,
                     uint64_t a2 = 0) {
  uint64_t result = 123;
  Require(GuestAudioOutputCall(name, a0, a1, a2, &result, Read, Write));
  return result;
}
int main() {
  // No speaker, synthetic tone, or real hardware is used: the host queue is
  // mocked.
  struct {
    uint32_t id = 0, canary = 0x12345678;
  } acm;
  Allow(&acm.id, 4);
  Require(Call("sceAcmContextCreate", reinterpret_cast<uint64_t>(&acm.id)) ==
              0 &&
          acm.id != 0 && acm.canary == 0x12345678);
  Require(Call("sceAcmContextCreate", 1) == uint64_t(bad_address));
  uint64_t user=0;Allow(&user,8);
  Require(Call("sceAudioOut2UserCreate",1,reinterpret_cast<uint64_t>(&user))==0 && user!=0);
  Require(Call("sceAudioOut2UserCreate",1,1)==uint64_t(bad_address));
  GuestAudioRegisterContext(5, 2);
  PortParam param{};
  param.format = 0x800;
  param.frequency = 48000;
  uint64_t handle = 0;
  Allow(&param, sizeof(param));
  Allow(&handle, 8);
  Require(Call("sceAudioOut2PortCreate", 5, reinterpret_cast<uint64_t>(&param),
               reinterpret_cast<uint64_t>(&handle)) == 0 &&
          handle != 0);
  Require(Call("sceAudioOut2PortCreate", 5, 1,
               reinterpret_cast<uint64_t>(&handle)) == uint64_t(bad_address));
  std::array<float, 16> samples{};
  Allow(samples.data(), sizeof(samples));
  const void *pcm = samples.data();
  Allow(&pcm, 8);
  Attribute attr{0, 0, &pcm, 8};
  Allow(&attr, sizeof(attr));
  Require(Call("sceAudioOut2PortSetAttributes", handle,
               reinterpret_cast<uint64_t>(&attr), 1) == 0);
  GuestAudioPushPcm(5, Read);
  Require(opens == 1 && writes == 1 && contexts.at(5)->nonzero == 0);
  samples[0] = 0.5f;
  samples[1] = 0.25f;
  samples[10] = 0.5f;
  GuestAudioPushPcm(5, Read);
  Require(opens == 1 && writes == 2);
  Require(pending[1]->dwBufferLength == 8);
  const auto *output = reinterpret_cast<const int16_t *>(pending[1]->lpData);
  Require(output[0] > 6000 && output[0] < 6100 && output[1] > 3000 &&
          output[1] < 3050);
  Require(output[2] > 4200 && output[2] < 4350 && output[2] == output[3]);
  samples.fill(0);
  Require(output[0] > 6000); // Host submission owns its copy.
  for (unsigned i = 0; i < 7; i++)
    GuestAudioPushPcm(5, Read);
  Require(writes == 8 && contexts.at(5)->drops == 1);
  const auto first = output[0];
  Require(first > 6000);
  for (auto *h : pending)
    h->dwFlags |= WHDR_DONE;
  GuestAudioPushPcm(5, Read);
  Require(writes == 9 && unprepares == 1);
  pcm = reinterpret_cast<void *>(1);
  GuestAudioPushPcm(5, Read);
  Require(contexts.at(5)->nonzero == 1);
  struct {
    PortState state;
    uint64_t canary = 0xabcdef;
  } status;
  Allow(&status.state, sizeof(status.state));
  Require(Call("sceAudioOut2PortGetState", handle,
               reinterpret_cast<uint64_t>(&status.state)) == 0);
  Require(status.state.channels == 8 && status.canary == 0xabcdef);
  std::array<float, 2> mono{0.5f, std::numeric_limits<float>::infinity()};
  std::array<float, 4> mix{};
  uint64_t nf = 0;
  Require(MixGuestAudioStereo(
      {reinterpret_cast<unsigned char *>(mono.data()), sizeof(mono)}, 0x100, 2,
      mix, &nf));
  Require(mix[0] == 0.5f && mix[1] == 0.5f && mix[2] == 0 && nf == 1);
  const auto before = mix;
  Require(!MixGuestAudioStereo({}, 0x300, 2, mix, &nf) && mix == before);
  std::array<int16_t, 2> signed_pcm{-32768, 16384};
  mix.fill(0);
  nf = 0;
  Require(
      MixGuestAudioStereo({reinterpret_cast<unsigned char *>(signed_pcm.data()),
                           sizeof(signed_pcm)},
                          0x201, 1, std::span<float>(mix.data(), 2), &nf));
  Require(mix[0] == -1.0f && mix[1] == 0.5f);
  // Object samples are measured, never mixed into the speaker bed without a
  // spatial renderer.
  uint64_t object = 0;
  PortParam object_param{};
  object_param.type = 0x100;
  object_param.format = 0x100;
  object_param.frequency = 48000;
  Allow(&object_param, sizeof(object_param));
  Allow(&object, 8);
  Require(Call("sceAudioOut2PortCreate", 5,
               reinterpret_cast<uint64_t>(&object_param),
               reinterpret_cast<uint64_t>(&object)) == 0);
  const void *object_pcm = mono.data();
  Allow(mono.data(), sizeof(mono));
  Allow(&object_pcm, 8);
  Attribute object_attr{0, 0, &object_pcm, 8};
  Allow(&object_attr, sizeof(object_attr));
  Require(Call("sceAudioOut2PortSetAttributes", object,
               reinterpret_cast<uint64_t>(&object_attr), 1) == 0);
  const auto prior_nonzero = contexts.at(5)->nonzero;
  GuestAudioPushPcm(5, Read);
  Require(contexts.at(5)->nonzero == prior_nonzero);
  Require(Call("sceAudioOut2PortSetAttributes", object, 1, 1) ==
          uint64_t(bad_address));
  std::array<float,8> lfe_only{};lfe_only[3]=0.75f;mix.fill(0);float raw_peak=0;
  Require(MixGuestAudioStereo({reinterpret_cast<unsigned char*>(lfe_only.data()),sizeof(lfe_only)},0x800,1,std::span<float>(mix.data(),2),&nf,&raw_peak));
  Require(raw_peak==0.75f && mix[0]==0 && mix[1]==0);
  GuestAudioDestroyContext(5);
  Require(closes == 1 && resets == 1 && ports.empty());
  std::puts("PASS: AudioOut2 ABI/handles, 7.1 downmix, finite PCM, invalid "
            "pointers, async ownership, bounded host queue and teardown; no "
            "hardware audio played");
}