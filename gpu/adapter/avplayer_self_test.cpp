#include "guest_avplayer.h"
#include "libs/avPlayerTypes.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <unordered_map>
#include "../../tests/avplayer_raw_audio_fixture.h"
using namespace Libs::Audio::AvPlayer;
namespace {
struct Fixture {
    FILE *file = nullptr;
    size_t opens = 0, closes = 0, reads = 0, allocs = 0, frees = 0, ready = 0, play = 0;
    std::unordered_map<void *, size_t> live;
};
void *KYTY_SYSV_ABI Alloc(void *object, uint32_t alignment, uint32_t size) {
    auto &f = *static_cast<Fixture *>(object);
    void *p = _aligned_malloc(size, std::max(alignment, 8u));
    if (p) {
        f.live[p] = size;
        f.allocs++;
    }
    return p;
}
void KYTY_SYSV_ABI Free(void *object, void *p) {
    auto &f = *static_cast<Fixture *>(object);
    if (p) {
        f.live.erase(p);
        f.frees++;
        _aligned_free(p);
    }
}
int KYTY_SYSV_ABI Open(void *object, const char *path) {
    auto &f = *static_cast<Fixture *>(object);
    f.file = fopen(path, "rb");
    if (f.file)
        f.opens++;
    return f.file ? 0 : -1;
}
int KYTY_SYSV_ABI Close(void *object) {
    auto &f = *static_cast<Fixture *>(object);
    if (f.file) {
        fclose(f.file);
        f.file = nullptr;
        f.closes++;
    }
    return 0;
}
int KYTY_SYSV_ABI ReadFile(void *object, uint8_t *data, uint64_t off, uint32_t size) {
    auto &f = *static_cast<Fixture *>(object);
    f.reads++;
    if (_fseeki64(f.file, off, SEEK_SET))
        return -1;
    return static_cast<int>(fread(data, 1, size, f.file));
}
uint64_t KYTY_SYSV_ABI FileSize(void *object) {
    auto &f = *static_cast<Fixture *>(object);
    const auto pos = _ftelli64(f.file);
    _fseeki64(f.file, 0, SEEK_END);
    const auto size = _ftelli64(f.file);
    _fseeki64(f.file, pos, SEEK_SET);
    return size;
}
void KYTY_SYSV_ABI Event(void *object, int32_t id, int32_t, void *) {
    auto &f = *static_cast<Fixture *>(object);
    if (id == AVPLAYER_EVENT_STATE_READY)
        f.ready++;
    if (id == AVPLAYER_EVENT_STATE_PLAY)
        f.play++;
}
bool Readable(const void *p, size_t bytes) {
    const auto a = reinterpret_cast<uintptr_t>(p);
    if (!a || !bytes || bytes > UINTPTR_MAX - a)
        return false;
    uintptr_t cursor = a;
    while (cursor < a + bytes) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void *>(cursor), &m, sizeof(m)) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        auto next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= cursor)
            return false;
        cursor = next;
    }
    return true;
}
bool Writable(void *p, size_t bytes) {
    if (!Readable(p, bytes))
        return false;
    auto cursor = reinterpret_cast<uintptr_t>(p);
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void *>(cursor), &m, sizeof(m)))
            return false;
        if (!(m.Protect &
              (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        cursor = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
    }
    return true;
}
uint64_t Call(const char *name, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    uint64_t out = 0xdeadbeef;
    if (!GuestAvPlayerCall(name, a, b, c, 0, &out, Readable, Writable))
        return UINT64_MAX;
    return out;
}
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            printf("FAIL AvPlayer line=%d: %s\n", __LINE__, #x);                                             \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
} // namespace
extern "C" int PsemuAvPlayerSelfTest(const char *path, const char *scratch_directory) {
    Fixture f{};
    AvPlayerInitData init{};
    init.memory_replacement = {&f, Alloc, Free, Alloc, Free};
    init.file_replacement = {&f, Open, Close, ReadFile, FileSize};
    init.event_replacement = {&f, Event};
    init.num_output_video_framebuffers = 2;
    CHECK(Call("sceAvPlayerInit", 1) == 0);
    auto bad = init;
    bad.memory_replacement.allocate = nullptr;
    CHECK(Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(&bad)) == 0);
    bad = init;
    bad.file_replacement.size = nullptr;
    CHECK(Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(&bad)) == 0);
    bad = init;
    bad.default_language = reinterpret_cast<const char *>(1);
    CHECK(Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(&bad)) == 0);
    const auto h = Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(&init));
    CHECK(h != 0);
    struct Cleanup {
        uint64_t h;
        ~Cleanup() { Call("sceAvPlayerClose", h); }
    } cleanup{h};
    CHECK(Call("sceAvPlayerIsActive", h) == 0);
    AvPlayerInitDataEx ex{};
    ex.this_size = sizeof(ex);
    ex.memory_replacement = init.memory_replacement;
    ex.file_replacement = init.file_replacement;
    ex.event_replacement = init.event_replacement;
    struct {
        uint64_t pre = 0x123456789abcdef0, id = 0, post = 0xfedcba9876543210;
    } exout;
    CHECK(Call("sceAvPlayerInitEx", reinterpret_cast<uint64_t>(&ex), reinterpret_cast<uint64_t>(&exout.id)) ==
          0);
    CHECK(exout.pre == 0x123456789abcdef0 && exout.post == 0xfedcba9876543210 && exout.id != 0);
    CHECK(Call("sceAvPlayerClose", exout.id) == 0);
    ex.this_size = 0;
    exout.id = 0xfeedface;
    CHECK(static_cast<int32_t>(Call("sceAvPlayerInitEx", reinterpret_cast<uint64_t>(&ex),
                                    reinterpret_cast<uint64_t>(&exout.id))) == AVPLAYER_ERROR_INVALID_PARAMS);
    CHECK(exout.id == 0xfeedface);
    auto *guarded =
        static_cast<uint8_t *>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(guarded);
    struct ReleasePages {
        void *p;
        ~ReleasePages() { VirtualFree(p, 0, MEM_RELEASE); }
    } release_pages{guarded};
    DWORD previous = 0;
    CHECK(VirtualProtect(guarded + 4096, 4096, PAGE_NOACCESS, &previous));
    CHECK(Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(guarded + 4096 - sizeof(init) + 1)) == 0);
    CHECK(Call("sceAvPlayerGetVideoDataEx", h, reinterpret_cast<uint64_t>(guarded + 4096 - 16)) == 0);
    CHECK(VirtualProtect(guarded, 4096, PAGE_READONLY, &previous));
    CHECK(static_cast<int32_t>(Call("sceAvPlayerGetStreamInfo", h, 0, reinterpret_cast<uint64_t>(guarded))) ==
          AVPLAYER_ERROR_INVALID_PARAMS);

    CHECK(static_cast<int32_t>(Call("sceAvPlayerGetStreamInfo", h, 0, 1)) == AVPLAYER_ERROR_INVALID_PARAMS);
    CHECK(static_cast<int32_t>(Call("sceAvPlayerAddSource", h, 1)) == AVPLAYER_ERROR_INVALID_PARAMS);
    CHECK(Call("sceAvPlayerAddSource", h, reinterpret_cast<uint64_t>(path)) == 0);
    CHECK(Call("sceAvPlayerStreamCount", h) >= 1);
    bool video = false, audio = false;
    for (uint32_t i = 0; i < Call("sceAvPlayerStreamCount", h); ++i) {
        struct {
            uint64_t pre = 0x123456789abcdef0;
            AvPlayerStreamInfo info{};
            uint64_t post = 0xfedcba9876543210;
        } canary;
        CHECK(Call("sceAvPlayerGetStreamInfo", h, i, reinterpret_cast<uint64_t>(&canary.info)) == 0);
        CHECK(canary.pre == 0x123456789abcdef0 && canary.post == 0xfedcba9876543210);
        video |= canary.info.type == AvPlayerStreamVideo;
        audio |= canary.info.type == AvPlayerStreamAudio;
    }
    CHECK(audio && f.opens == 1 && f.ready == 1);
    CHECK(Call("sceAvPlayerStart", h) == 0);
    CHECK(f.play == 1);
    CHECK(Call("sceAvPlayerGetVideoDataEx", h, 1) == 0);
    struct {
        uint64_t pre = 0x0123456789abcdef;
        AvPlayerFrameInfoEx info{};
        uint64_t post = 0xfedcba9876543210;
    } frame;
    if (video) {
        CHECK(Call("sceAvPlayerGetVideoDataEx", h, reinterpret_cast<uint64_t>(&frame.info)) == 1);
        CHECK(frame.pre == 0x0123456789abcdef && frame.post == 0xfedcba9876543210);
        CHECK(frame.info.data && frame.info.details.video.width && frame.info.details.video.height);
    } else {
        CHECK(Call("sceAvPlayerGetVideoDataEx", h, reinterpret_cast<uint64_t>(&frame.info)) == 0);
    }
    uint64_t nonzero = 0, pcm_bytes = 0;
    size_t blocks = 0;
    for (size_t i = 0; i < 500; ++i) {
        struct {
            uint64_t pre = 0x123456789abcdef0;
            AvPlayerFrameInfo info{};
            uint64_t post = 0xfedcba9876543210;
        } pcm;
        if (!Call("sceAvPlayerGetAudioData", h, reinterpret_cast<uint64_t>(&pcm.info)))
            break;
        CHECK(pcm.pre == 0x123456789abcdef0 && pcm.post == 0xfedcba9876543210);
        CHECK(pcm.info.details.audio.sample_rate == 48000 && pcm.info.details.audio.channel_count == 2);
        auto it = f.live.find(pcm.info.data);
        CHECK(it != f.live.end() && pcm.info.details.audio.size <= it->second);
        const auto *samples = static_cast<const int16_t *>(pcm.info.data);
        for (size_t j = 0; j < pcm.info.details.audio.size / 2; ++j)
            nonzero += samples[j] != 0;
        pcm_bytes += pcm.info.details.audio.size;
        ++blocks;
        if (nonzero)
            break;
    }
    CHECK(blocks && pcm_bytes);
    uint64_t raw_blocks = 0, raw_nonzero = 0;
    CHECK(RawAudioNonzero(path, raw_blocks, raw_nonzero));
    CHECK((nonzero != 0) == (raw_nonzero != 0));
    printf("[AVPLAYER-SOURCE-CHECK] raw_decode_blocks=%llu raw_nonzero=%llu production_pcm_blocks=%zu "
           "pcm_nonzero=%llu\n",
           raw_blocks, raw_nonzero, blocks, nonzero);
    CHECK(Call("sceAvPlayerStop", h) == 0);
    CHECK(Call("sceAvPlayerClose", h) == 0);
    CHECK(f.live.empty() && f.allocs == f.frees && f.closes == 1 && f.reads > 0);
    CHECK(static_cast<int32_t>(Call("sceAvPlayerClose", h)) == AVPLAYER_ERROR_INVALID_PARAMS);
    CHECK(Call("sceAvPlayerIsActive", h) == 0);
    const auto h2 = Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(&init));
    CHECK(h2 && h2 != h);
    CHECK(Call("sceAvPlayerClose", h2) == 0);
    printf("[AVPLAYER-CODECS] pcm_f32le=%d pcm_s16le=%d aac_encoder=%d\n",
           avcodec_find_decoder(AV_CODEC_ID_PCM_F32LE) != nullptr,
           avcodec_find_decoder(AV_CODEC_ID_PCM_S16LE) != nullptr,
           avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr);
    // Explicit scratch directory: do not use the OS/user temp location.
    CHECK(std::filesystem::path(scratch_directory).is_absolute());
    CHECK(std::filesystem::is_directory(scratch_directory));
    const auto wav = (std::filesystem::path(scratch_directory) /
                      ("psemu-avplayer-pcm-" + std::to_string(GetCurrentProcessId()) + ".wav"))
                         .string();
    CHECK(!std::filesystem::exists(wav));
    FILE *out = fopen(wav.c_str(), "wb");
    CHECK(out);
    const uint32_t chunk = 36 + 1024 * 4, fmt_size = 16, rate = 48000, byte_rate = rate * 4, bytes = 1024 * 4;
    const uint16_t tag = 1, channels = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, out);
    fwrite(&chunk, 4, 1, out);
    fwrite("WAVEfmt ", 1, 8, out);
    fwrite(&fmt_size, 4, 1, out);
    fwrite(&tag, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&rate, 4, 1, out);
    fwrite(&byte_rate, 4, 1, out);
    fwrite(&align, 2, 1, out);
    fwrite(&bits, 2, 1, out);
    fwrite("data", 1, 4, out);
    fwrite(&bytes, 4, 1, out);
    const int16_t pair[2] = {16384, -8192};
    for (int i = 0; i < 1024; ++i)
        fwrite(pair, sizeof(pair), 1, out);
    fclose(out);
    struct RemoveFixture {
        std::string path;
        ~RemoveFixture() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } remove{wav};
    const auto hw = Call("sceAvPlayerInit", reinterpret_cast<uint64_t>(&init));
    CHECK(hw);
    Cleanup clean_wav{hw};
    CHECK(Call("sceAvPlayerAddSource", hw, reinterpret_cast<uint64_t>(wav.c_str())) == 0);
    CHECK(Call("sceAvPlayerStart", hw) == 0);
    AvPlayerFrameInfo wav_pcm{};
    CHECK(Call("sceAvPlayerGetAudioData", hw, reinterpret_cast<uint64_t>(&wav_pcm)) == 1);
    CHECK(wav_pcm.details.audio.size > 0 && wav_pcm.details.audio.channel_count == 2);
    const auto *converted = static_cast<const int16_t *>(wav_pcm.data);
    for (size_t i = 0; i < wav_pcm.details.audio.size / 4; ++i)
        CHECK(converted[i * 2] == 16384 && converted[i * 2 + 1] == -8192);
    CHECK(Call("sceAvPlayerClose", hw) == 0);
    CHECK(f.live.empty() && f.allocs == f.frees && f.opens == f.closes);
    printf("PASS AvPlayer nonzero s16 exact samples, no host playback\n");
    printf("PASS AvPlayer production bridge: real file callbacks, native decode=%ux%u, PCM blocks=%zu "
           "bytes=%llu nonzero_s16=%llu, ABI canaries, invalid pointers/handles, owned allocations=%zu "
           "freed=%zu. Astro guest=not-executed, playback=not-executed\n",
           frame.info.details.video.width, frame.info.details.video.height, blocks, pcm_bytes, nonzero,
           f.allocs, f.frees);
    return 0;
}