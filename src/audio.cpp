// ============================================================================
// psemu - ses cikisi (sceAudioOut*) : waveOut arka ucu
// ----------------------------------------------------------------------------
// OLCUM (Dreaming Sarah, dsH.out):
//   sceAudioOutInit       x1
//   sceAudioOutOpen       x1     <- UYGULANMAMIS, varsayilan stub RAX=0 donuyordu
//   sceAudioOutSetVolume  x1     <- UYGULANMAMIS
//   sceAudioOutOutput     x9000+ <- yalnizca Sleep(5) yapip VERIYI COPE ATIYORDU
// Yani oyun sesi kesintisiz uretiyordu; biz hicbir zaman calmiyorduk.
// Oyunun sessiz olmasinin sebebi buydu.
//
// IKI GERCEK HATA DAHA:
//  1) sceAudioOutOpen 0 donuyordu. Sony'de handle > 0'dir; 0 birçok oyunda
//     gecersiz port sayilir.
//  2) sceAudioOutOutput'un UZUNLUK ARGUMANI YOKTUR - imza (handle, ptr).
//     Eski kod RDX'i "num" sanip okuyordu; RDX orada cagridan kalma coptu
//     (tesaduf eseri 256 gorunuyordu). Dogru uzunluk Open'daki len'dir,
//     bu yuzden Open'i uygulamak pacing icin de sart.
// ============================================================================
#include "audio.h"

#include <windows.h>

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace Psemu::Audio {
namespace {

constexpr int kMaxPorts = 8;
// 8 tampon x grain. 256 ornek @48 kHz -> ~5.33 ms, yani ~43 ms tamponlama.
// Daha az: cizirti (underrun). Daha cok: gereksiz gecikme.
constexpr int kBuffers = 8;

// SCE_AUDIO_OUT_PARAM_FORMAT_*
enum : uint32_t {
    FMT_S16_MONO      = 0,
    FMT_S16_STEREO    = 1,
    FMT_S16_8CH       = 2,
    FMT_FLOAT_MONO    = 3,
    FMT_FLOAT_STEREO  = 4,
    FMT_FLOAT_8CH     = 5,
    FMT_S16_8CH_STD   = 6,
    FMT_FLOAT_8CH_STD = 7,
};

struct Port {
    bool     used    = false;
    HWAVEOUT hwo     = nullptr;
    HANDLE   done_ev = nullptr;
    WAVEHDR  hdr[kBuffers]{};
    std::vector<int16_t> buf[kBuffers];
    int      next     = 0;
    uint32_t len      = 256;   // grain: KANAL BASINA ornek
    uint32_t freq     = 48000;
    uint32_t src_ch   = 2;
    uint32_t out_ch   = 2;
    bool     is_float = false;
    // Yazilim kazanci. waveOutSetVolume yerine burada uyguluyoruz: Sony'nin
    // ses seviyesi KANAL BASINA, waveOut'unki ise yalnizca L/R.
    float      gain = 1.0f;
    std::mutex m;
};

Port  g_ports[kMaxPorts];
std::mutex g_ports_mutex;
std::atomic<bool> g_disabled{false};
std::atomic<uint64_t> g_frames{0};

inline int16_t ClampS16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return static_cast<int16_t>(v);
}

void DescribeFormat(uint32_t fmt, uint32_t* ch, bool* is_float) {
    switch (fmt) {
        case FMT_S16_MONO:      *ch = 1; *is_float = false; break;
        case FMT_S16_STEREO:    *ch = 2; *is_float = false; break;
        case FMT_S16_8CH:
        case FMT_S16_8CH_STD:   *ch = 8; *is_float = false; break;
        case FMT_FLOAT_MONO:    *ch = 1; *is_float = true;  break;
        case FMT_FLOAT_STEREO:  *ch = 2; *is_float = true;  break;
        case FMT_FLOAT_8CH:
        case FMT_FLOAT_8CH_STD: *ch = 8; *is_float = true;  break;
        default:                *ch = 2; *is_float = false; break;
    }
}

// PS4/PS5 8 kanal sirasi: FL FR C LFE RL RR SL SR.
// ITU-R BS.775 asagi karisimi ile stereo'ya indiriyoruz (host cihazlarin
// cogu stereo; 8 kanal acmaya calismak waveOutOpen'i basarisiz kilar).
inline void Downmix8(const float* s, float* l, float* r) {
    const float c = s[2] * 0.707f;
    const float e = s[3] * 0.500f;
    *l = s[0] + c + e + 0.707f * s[4] + 0.707f * s[6];
    *r = s[1] + c + e + 0.707f * s[5] + 0.707f * s[7];
}

void Convert(const Port& p, const void* src, int16_t* dst) {
    const uint32_t n = p.len;
    const float    g = p.gain;

    if (p.src_ch == p.out_ch) {
        const uint32_t total = n * p.out_ch;
        if (p.is_float) {
            const auto* s = static_cast<const float*>(src);
            for (uint32_t i = 0; i < total; i++) dst[i] = ClampS16(s[i] * 32767.0f * g);
        } else if (g >= 0.999f) {
            memcpy(dst, src, static_cast<size_t>(total) * sizeof(int16_t));
        } else {
            const auto* s = static_cast<const int16_t*>(src);
            for (uint32_t i = 0; i < total; i++) dst[i] = ClampS16(static_cast<float>(s[i]) * g);
        }
        return;
    }

    // 8 -> 2 asagi karisimi.
    float f[8];
    for (uint32_t i = 0; i < n; i++) {
        if (p.is_float) {
            const auto* s = static_cast<const float*>(src) + static_cast<size_t>(i) * 8;
            for (int k = 0; k < 8; k++) f[k] = s[k] * 32767.0f;
        } else {
            const auto* s = static_cast<const int16_t*>(src) + static_cast<size_t>(i) * 8;
            for (int k = 0; k < 8; k++) f[k] = static_cast<float>(s[k]);
        }
        float l, r;
        Downmix8(f, &l, &r);
        dst[i * 2 + 0] = ClampS16(l * g);
        dst[i * 2 + 1] = ClampS16(r * g);
    }
}

Port* Resolve(int handle) {
    const int i = handle - 1;
    if (i < 0 || i >= kMaxPorts) return nullptr;
    Port* p = &g_ports[i];
    return p->used ? p : nullptr;
}

// Cihaz yokken/kapaliyken pacing'i koruyan uyku. Gercek donanimda Output
// bloklar; bloklamazsa ses thread'i bosa donup CPU'yu bogar (bu emulatorde
// tam olarak bu yasandi: tum PLT cagrilarinin ~%25'i).
inline void PaceSleep(uint32_t len, uint32_t freq) {
    if (freq == 0) freq = 48000;
    DWORD ms = static_cast<DWORD>((static_cast<uint64_t>(len) * 1000ull) / freq);
    if (ms == 0) ms = 1;
    Sleep(ms);
}

} // namespace

uint32_t DefaultGrain() { return 256; }

uint32_t PacketBytes(int handle) {
    Port* p = Resolve(handle);
    if (p == nullptr) return 0;
    return p->len * p->src_ch * (p->is_float ? 4u : 2u);
}

int Init() {
    const char* off = getenv("PSEMU_NO_AUDIO");
    if (off != nullptr && off[0] == '1') {
        g_disabled.store(true, std::memory_order_relaxed);
        printf("[AUDIO] PSEMU_NO_AUDIO=1 - ses cihazi acilmayacak (yalnizca pacing)\n");
        fflush(stdout);
    }
    return 0;
}

int Open(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param) {
    uint32_t fmt = param & 0x1Fu;
    uint32_t src_ch;
    bool     is_float;
    DescribeFormat(fmt, &src_ch, &is_float);

    if (len == 0 || len > 8192) len = DefaultGrain();
    if (freq == 0) freq = 48000;

    Port* p = nullptr;
    int   handle = 0;
    {
        std::lock_guard<std::mutex> lk(g_ports_mutex);
        for (int i = 0; i < kMaxPorts; i++) {
            if (!g_ports[i].used) {
                g_ports[i].used = true;
                p = &g_ports[i];
                handle = i + 1; // 0 DONMUYORUZ: gecersiz port sayilabilir
                break;
            }
        }
    }
    if (p == nullptr) return kErrorInvalidPort;

    p->len      = len;
    p->freq     = freq;
    p->src_ch   = src_ch;
    p->out_ch   = (src_ch == 1) ? 1u : 2u; // 8 kanal -> stereo asagi karisimi
    p->is_float = is_float;
    p->next     = 0;
    p->gain     = 1.0f;

    printf("[AUDIO] Open user=%d type=%d index=%d len=%u freq=%u param=0x%x "
           "-> bicim=%u kanal=%u%s -> handle=%d\n",
           user_id, type, index, len, freq, param, fmt, src_ch,
           is_float ? " float" : " s16", handle);
    fflush(stdout);

    if (g_disabled.load(std::memory_order_relaxed)) return handle;

    WAVEFORMATEX wf{};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = static_cast<WORD>(p->out_ch);
    wf.nSamplesPerSec  = freq;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = static_cast<WORD>(p->out_ch * 2);
    wf.nAvgBytesPerSec = freq * wf.nBlockAlign;

    p->done_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    MMRESULT mr = waveOutOpen(&p->hwo, WAVE_MAPPER, &wf,
                              reinterpret_cast<DWORD_PTR>(p->done_ev), 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR) {
        // Cihaz yoksa oyunu DURDURMUYORUZ: gecerli handle donup pacing'e
        // geri dusuyoruz. Sessiz ama calisir kalir.
        printf("[AUDIO] waveOutOpen basarisiz (mm=%u) - pacing'e geri dusuluyor\n",
               static_cast<unsigned>(mr));
        fflush(stdout);
        p->hwo = nullptr;
        if (p->done_ev) { CloseHandle(p->done_ev); p->done_ev = nullptr; }
        return handle;
    }

    const size_t samples = static_cast<size_t>(len) * p->out_ch;
    for (int i = 0; i < kBuffers; i++) {
        p->buf[i].assign(samples, 0);
        p->hdr[i] = WAVEHDR{};
        p->hdr[i].lpData         = reinterpret_cast<LPSTR>(p->buf[i].data());
        p->hdr[i].dwBufferLength = static_cast<DWORD>(samples * sizeof(int16_t));
        waveOutPrepareHeader(p->hwo, &p->hdr[i], sizeof(WAVEHDR));
        p->hdr[i].dwUser = 0; // 0 = hic kuyruga girmedi
    }

    printf("[AUDIO] waveOut ACIK: %u Hz, %u kanal, %d x %u ornek tampon (~%.1f ms)\n",
           freq, p->out_ch, kBuffers, len,
           1000.0 * kBuffers * len / static_cast<double>(freq));
    fflush(stdout);
    return handle;
}

int Output(int handle, const void* pcm) {
    Port* p = Resolve(handle);
    if (p == nullptr) {
        // Gecersiz port: yine de pace'le, yoksa oyun bu cagriyi sikistirip
        // CPU'yu bogar.
        PaceSleep(DefaultGrain(), 48000);
        return kErrorInvalidPort;
    }

    std::lock_guard<std::mutex> lk(p->m);

    if (p->hwo == nullptr || pcm == nullptr) {
        PaceSleep(p->len, p->freq);
        return static_cast<int>(p->len);
    }

    WAVEHDR* h = &p->hdr[p->next];

    // Sirasi gelen tamponun calinmasini BEKLE. Bloklama buradan gelir ve
    // ses thread'ini dogal olarak ornekleme hizina pace'ler - sabit Sleep
    // yerine gercek tuketim hizi.
    if (h->dwUser != 0) {
        DWORD waited = 0;
        while ((h->dwFlags & WHDR_DONE) == 0) {
            if (WaitForSingleObject(p->done_ev, 50) == WAIT_TIMEOUT) {
                waited += 50;
                if (waited >= 500) {
                    // Cihaz takildi. EMULATORU ASLA KILITLEME: paketi dusur,
                    // pacing uykusuyla devam et.
                    PaceSleep(p->len, p->freq);
                    return static_cast<int>(p->len);
                }
            }
        }
    }

    Convert(*p, pcm, p->buf[p->next].data());

    h->dwFlags &= ~WHDR_DONE;
    if (waveOutWrite(p->hwo, h, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
        h->dwUser = 1;
    }
    p->next = (p->next + 1) % kBuffers;

    const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 4000ull) == 0) {
        printf("[AUDIO] %llu paket calindi (%u ornek/paket)\n",
               static_cast<unsigned long long>(n), p->len);
        fflush(stdout);
    }
    return static_cast<int>(p->len);
}

int SetVolume(int handle, uint32_t flag, const int* vol) {
    Port* p = Resolve(handle);
    if (p == nullptr) return kErrorInvalidPort;
    if (vol == nullptr) return 0;

    // Sony: kanal basina 0..32768 (32768 = 0 dB). flag kanal maskesi.
    // Tek bir yazilim kazanci istedigimiz icin isaretli kanallarin EN
    // BUYUGUNU aliyoruz (kismi susturmayi yanlislikla her seye uygulamamak
    // icin ortalama degil maksimum).
    int best = 0;
    for (int c = 0; c < 8; c++) {
        if ((flag & (1u << c)) == 0) continue;
        if (vol[c] > best) best = vol[c];
    }
    if (best <= 0 && flag != 0) best = 0;
    float g = static_cast<float>(best) / 32768.0f;
    if (g > 4.0f) g = 4.0f;

    std::lock_guard<std::mutex> lk(p->m);
    p->gain = g;
    printf("[AUDIO] SetVolume handle=%d flag=0x%x -> kazanc=%.3f\n", handle, flag, g);
    fflush(stdout);
    return 0;
}

int Close(int handle) {
    Port* p = Resolve(handle);
    if (p == nullptr) return kErrorInvalidPort;

    std::lock_guard<std::mutex> lk(p->m);
    if (p->hwo != nullptr) {
        waveOutReset(p->hwo);
        for (int i = 0; i < kBuffers; i++) {
            waveOutUnprepareHeader(p->hwo, &p->hdr[i], sizeof(WAVEHDR));
        }
        waveOutClose(p->hwo);
        p->hwo = nullptr;
    }
    if (p->done_ev != nullptr) {
        CloseHandle(p->done_ev);
        p->done_ev = nullptr;
    }
    p->used = false;
    printf("[AUDIO] Close handle=%d\n", handle);
    fflush(stdout);
    return 0;
}

} // namespace Psemu::Audio
