#include "agc.h"
#include "core.h"    // g_dmem_base_addr (phys -> CPU cevirisi)
#include "video.h"
#include "logger.h"

#include <sstream>
#include <atomic>
#include <thread>
#include <cstdint>

// Kyty graphics init: gpu/adapter/init.cpp icinde (Kyty header'lariyla). Kyty
// subsystem'lerini baslatir + WindowInit/WindowRun (VulkanCreate) yapar. psemu
// cekirdegi gpu include yollarina sahip olmadigi icin yalnizca bu tek fonksiyonu
// forward-declare edip cagiriyoruz; linker gpu lib'den cozer ve ilgili objeleri
// (subsystem'ler, window_win32 -> VulkanCreate -> renderer) ARTIMLI ceker.
void PsemuInitKytyGraphics();

// ============================================================================
// AGC HLE-GPU — M1: oyun-guduml flip + render-state yakalama
// ----------------------------------------------------------------------------
// Onemli guvenlik notu: bu AGC fonksiyonlari onceden generic stub ile 0
// donuyordu ve oyun bu haliyle render loop'una kadar CALISIYORDU. Bu modul de
// hepsini 0 dondurur (ayni davranis) -> M1 hicbir seyi bozamaz. Tek eklenen
// davranis: sceAgcDcbSetFlip artik Video::Flip'i cagirarak sunumu GERCEKTEN
// oyun frame'lerine baglar. Ayrica cizim/shader/flip sayilarini loglayarak bir
// sonraki adim (clear rengi + sprite raster) icin kaniti biriktiriyoruz.
// ============================================================================

namespace Agc {

namespace {

// Guest bellegini C++ nesnesi icermeyen ayri bir fonksiyonda SEH ile guvenli
// kopyalar (gecersiz pointer'da cokmek yerine false doner). /EHsc ile SEH
// karisimi sorun cikarmasin diye burada hicbir yikilabilir nesne YOK.
bool SafeCopy(void* dst, uint64_t guest, size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(guest), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Bir guest bellek bolgesini hex olarak loglar (doku/descriptor kesfi icin).
void DumpMem(const char* etiket, uint64_t addr, size_t n) {
    if (addr == 0) { LOG_INFO(std::string("[AGC-DUMP] ") + etiket + ": adres 0"); return; }
    unsigned char buf[256];
    if (n > sizeof(buf)) n = sizeof(buf);
    if (!SafeCopy(buf, addr, n)) {
        std::stringstream ss;
        ss << "[AGC-DUMP] " << etiket << " @0x" << std::hex << addr << ": OKUNAMADI";
        LOG_INFO(ss.str());
        return;
    }
    std::stringstream ss;
    ss << "[AGC-DUMP] " << etiket << " @0x" << std::hex << addr << ":";
    for (size_t i = 0; i < n; i++) {
        if ((i & 15) == 0) ss << "\n  +" << std::hex << i << ": ";
        ss << std::hex << (buf[i] < 16 ? "0" : "") << static_cast<int>(buf[i]) << " ";
    }
    LOG_INFO(ss.str());
}

// Draw oncesi yakalanan kaynak adresleri (register indirect bloklari + index).
uint64_t g_sh_block = 0;   // shader resource block (doku T# sharp burada)
uint64_t g_uc_block = 0;   // user-config / vertex block (V# sharp burada)
uint64_t g_index_buf = 0;  // index buffer adresi

// Bir bellek bolgesi "goruntu verisi gibi mi?" — sifir degil ve yeterince
// cesitli (tek renk/sabit degil). Doku aday tespiti icin kaba sezgi.
bool LooksLikeImage(uint64_t cpu_addr, uint64_t* out_variety) {
    unsigned char buf[64];
    if (!SafeCopy(buf, cpu_addr, sizeof(buf))) { if (out_variety) *out_variety = 0; return false; }
    int nonzero = 0, distinct_bits = 0; unsigned char acc = 0;
    for (unsigned char b : buf) { if (b) nonzero++; acc |= b; }
    for (int i = 0; i < 8; i++) if (acc & (1 << i)) distinct_bits++;
    if (out_variety) *out_variety = static_cast<uint64_t>(nonzero);
    return nonzero >= 16 && distinct_bits >= 4; // yarisi dolu + bit cesitliligi
}

// SH/UC blogundaki her 32-bit degeri OLASI FIZIKSEL ADRES kabul edip CPU'ya
// cevirir (3 yorum: ham-CPU, phys, phys>>8) ve goruntu gibi gorunen adaylari
// loglar. Amac: RDNA2 descriptor formatini bilmeden dokunun base adresini
// AMPIRIK bulmak. Bulunca M3'te o dokuyu framebuffer'a basacagiz.
void ScanForTexture(const char* etiket, uint64_t block_addr) {
    if (block_addr == 0 || g_dmem_base_addr == 0) return;
    uint32_t words[64];
    if (!SafeCopy(words, block_addr, sizeof(words))) return;
    const uint64_t pool = g_dmem_base_addr;
    const uint64_t pool_end = pool + 0x100000000ULL;
    for (int i = 0; i < 64; i++) {
        uint32_t v = words[i];
        if (v == 0) continue;
        struct { const char* how; uint64_t addr; } cands[3] = {
            { "raw-CPU", static_cast<uint64_t>(v) },                 // dogrudan CPU (dusuk 32?)
            { "phys",    pool + v },                                 // phys offset
            { "phys<<8", pool + (static_cast<uint64_t>(v) << 8) },   // descriptor base>>8
        };
        for (auto& c : cands) {
            if (c.addr < pool || c.addr >= pool_end) continue; // havuz disi ele
            uint64_t variety = 0;
            if (LooksLikeImage(c.addr, &variety)) {
                std::stringstream ss;
                ss << "[AGC-TEX] " << etiket << " word[" << i << "]=0x" << std::hex << v
                   << " -> " << c.how << " CPU=0x" << c.addr
                   << " GORUNTU-ADAYI (nonzero=" << std::dec << variety << "/64)";
                LOG_INFO(ss.str());
            }
        }
    }
}

// Bir fonksiyonun ilk `limit` cagrisini loglar, sonrasini susturur (log spam'i
// onlemek icin — AGC fonksiyonlari saniyede yuzlerce kez cagrilir).
bool LogFirst(const char* tag, int limit) {
    static std::atomic<int> counters[8]{};
    // basit hash: tag'in ilk 2 karakteri -> slot
    int slot = (static_cast<unsigned char>(tag[0]) + (tag[1] ? static_cast<unsigned char>(tag[1]) : 0)) & 7;
    int n = counters[slot].fetch_add(1) + 1;
    return n <= limit;
}

// Frame basina cizim sayaci (flip'te sifirlanir) — "her frame kac quad?" kaniti.
std::atomic<uint64_t> g_draws_this_frame{0};
std::atomic<uint64_t> g_total_draws{0};
std::atomic<uint64_t> g_shaders_created{0};

} // namespace

// Kyty graphics subsystem'ini BIR KEZ baslatir: WindowInit + ayri thread'de
// WindowRun (Win32 pencere + VulkanCreate: instance/device/swapchain). Ilk AGC
// cagrisinda tetiklenir. Bu, gpu lib objelerini ilk kez referans edip linke
// ceker; statik-init veya Vulkan-device hatalari BURADA ortaya cikacak.
void EnsureKytyGraphicsInit() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;

    LOG_INFO("[KYTY-GFX] Baslatiliyor: Kyty subsystem'leri + WindowInit + VulkanCreate...");
    PsemuInitKytyGraphics(); // subsystem init + WindowInit + WindowRun + wait
    LOG_INFO("[KYTY-GFX] Vulkan HAZIR (instance/device/swapchain kuruldu).");
}

bool Dispatch(const std::string& name, CONTEXT* ctx) {
    // Sadece AGC yuzeyini sahipleniyoruz: sceAgc* ve Graphics* (AGC yardimcilari).
    const bool is_agc =
        name.rfind("sceAgc", 0) == 0 || name.rfind("Graphics", 0) == 0;
    if (!is_agc) return false;

    // Ilk AGC cagrisinda Kyty Vulkan'ini tetikle (bir kez).
    EnsureKytyGraphicsInit();

    if (name == "sceAgcDcbSetFlip") {
        // sceAgcDcbSetFlip(dcb, videoOutHandle, displayBufferIndex, flipMode, ...)
        //   RDI=dcb, RSI=handle, RDX=buffer_index (0/1 -> cift buffer), RCX=mode
        // Frame sinirini burada aliyoruz: kayitli framebuffer'i pencereye sun.
        int buf = static_cast<int>(ctx->Rdx);
        uint64_t draws = g_draws_this_frame.exchange(0);
        Video::Flip(buf);
        if (LogFirst("FL", 12)) {
            std::stringstream ss;
            ss << "[AGC] SetFlip -> Video::Flip(buffer=" << buf
               << ")  (bu frame'de " << draws << " cizim/quad)";
            LOG_INFO(ss.str());
        }
        ctx->Rax = 0;
        return true;
    }

    // Kaynak adreslerini yakala (M2: dokuyu/vertex'i bulmak icin).
    // GraphicsSetShRegIndirectPatchSetAddress(dcb_slot, block_addr, size, ...)
    //   RDI=slot, RSI=block adresi (shader kaynak tablosu -> doku T# sharp)
    if (name == "GraphicsSetShRegIndirectPatchSetAddress") {
        g_sh_block = ctx->Rsi;
        ctx->Rax = 0;
        return true;
    }
    if (name == "GraphicsSetUcRegIndirectPatchSetAddress") {
        g_uc_block = ctx->Rsi;  // vertex/user-config blok (V# sharp)
        ctx->Rax = 0;
        return true;
    }
    if (name == "sceAgcDcbSetIndexBuffer") {
        // sceAgcDcbSetIndexBuffer(dcb, indexAddr, ...) -> RSI=index buffer
        g_index_buf = ctx->Rsi;
        ctx->Rax = 0;
        return true;
    }

    if (name == "sceAgcDcbDrawIndexOffset") {
        // sceAgcDcbDrawIndexOffset(dcb, indexOffset, indexCount, ...)
        //   RDI=dcb, RSI=index_offset, RDX=index_count (6 = 1 quad)
        uint64_t idx_count = ctx->Rdx;
        g_draws_this_frame.fetch_add(1);
        uint64_t total = g_total_draws.fetch_add(1) + 1;
        if (LogFirst("DR", 12)) {
            std::stringstream ss;
            ss << "[AGC] DrawIndexOffset #" << total
               << "  index_count=" << idx_count
               << (idx_count == 6 ? "  (1 quad/sprite)" : "");
            LOG_INFO(ss.str());
        }
        // (M2 dump/ScanForTexture kaldirildi: dokuyu ELLE bulmak icindi; artik
        //  Kyty'nin gercek GPU renderer'ini port ediyoruz. Ayrica SafeCopy'nin
        //  __try/__except'i clang-cl /EHsc altinda donanim exception'lari
        //  yakalamiyordu ve tarama crash ediyordu.)
        ctx->Rax = 0;
        return true;
    }

    if (name == "GraphicsCreateShader") {
        // GraphicsCreateShader(...) — PSSL shader nesnesi olusturur. Simdilik
        // sadece sayiyoruz; ileride shader ELF'ini cikarip SPIR-V'e cevirecegiz.
        uint64_t n = g_shaders_created.fetch_add(1) + 1;
        if (LogFirst("SH", 12)) {
            std::stringstream ss;
            ss << "[AGC] GraphicsCreateShader #" << n
               << "  (RDI=0x" << std::hex << ctx->Rdi
               << " RSI=0x" << ctx->Rsi << ")";
            LOG_INFO(ss.str());
        }
        ctx->Rax = 0;
        return true;
    }

    // Diger tum AGC/Graphics fonksiyonlari: no-op basari (0). Bu, onceki generic
    // stub ile AYNI davranis — sadece artik isimle cozuluyor (temiz log) ve tek
    // yerden yonetiliyor. Gercek implementasyonlar sonraki milestone'larda.
    ctx->Rax = 0;
    return true;
}

} // namespace Agc
