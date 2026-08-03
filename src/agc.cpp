#include "agc.h"
#include "core.h"    // g_dmem_base_addr (phys -> CPU cevirisi)
#include "video.h"
#include "logger.h"

#include <sstream>
#include <atomic>
#include <thread>
#include <cstdint>
#include <mutex>   // [AGC-YOK] tanisi
#include <set>     // [AGC-YOK] tanisi

// Kyty graphics init: gpu/adapter/init.cpp icinde (Kyty header'lariyla). Kyty
// subsystem'lerini baslatir + WindowInit/WindowRun (VulkanCreate) yapar. psemu
// cekirdegi gpu include yollarina sahip olmadigi icin yalnizca bu tek fonksiyonu
// forward-declare edip cagiriyoruz; linker gpu lib'den cozer ve ilgili objeleri
// (subsystem'ler, window_win32 -> VulkanCreate -> renderer) ARTIMLI ceker.
void PsemuInitKytyGraphics();

// AGC/VideoOut/Graphics cagrisini Kyty implementasyonuna yonlendiren kopru
// (gpu/adapter/agc_bridge.cpp). Suffix'siz NID ile Kyty fonksiyonunu bulup
// SysV ABI ile cagirir; bulursa true.
extern "C" bool PsemuKytyAgcCall(const char* nid, CONTEXT* ctx);
extern "C" bool PsemuKytyHasNid(const char* nid);

// ============================================================================
// AGC HLE-GPU — M1: oyun-guduml flip + render-state yakalama
// ----------------------------------------------------------------------------
// Onemli guvenlik notu: bu AGC fonksiyonlari onceden generic stub ile 0
// donuyordu ve oyun bu haliyle render loop'una kadar CALISIYORDU. Bu modul de
// hepsini 0 dondurur (ayni davranis) -> M1 hicbir seyi bozamaz. Tek eklenen
// davranis: sceAgcDcbSetFlip artik Video::Flip'i cagirarak sunumu GERCEKTEN
// oyun frame'lerine baglar. Ayrica cizim/shader/flip sayilarini loglayarak bir
// profil cikarir; ilerideki donanim emulasyonlarina temel olusturur.
// ============================================================================

namespace Agc {

namespace {

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
    static std::atomic<bool> finished{false};

    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
        // Init'i BASKA bir thread yapiyor: BITENE KADAR BEKLE.
        //
        // Eskiden burada dogrudan "return" vardi. Tek thread'li oyunlarda
        // (Dreaming Sarah) fark etmiyordu, ama Astro Bot 19 thread aciyor ve
        // birkaci ayni anda sceVideoOut*/sceAgc* cagiriyor. Beklemeden donen
        // thread, g_video_out_context henuz kurulmamisken grafik cagrisina
        // girip "EXIT_IF(g_video_out_context == nullptr)" fatal'ini
        // tetikliyordu (videoOut.cpp:1017, VideoOutEndVblank).
        while (!finished.load(std::memory_order_acquire)) {
            Sleep(1);
        }
        return;
    }

    LOG_INFO("[KYTY-GFX] Baslatiliyor: Kyty subsystem'leri + WindowInit + VulkanCreate...");

    // ONEMLI: init'i AYRI BIR THREAD'de yapiyoruz.
    // Buraya VEH (exception handler) BAGLAMINDAN geliyoruz: Dispatch, PLT
    // fault'unu isleyen handler'in icinden cagriliyor. Agir Win32/Vulkan/CRT
    // init'ini exception baglaminda yapmak, ayni anda VEH'e giren DIGER guest
    // thread'leriyle kilit ters siralamasi uretiyordu: launch'larin ~%50'si
    // "Initializing: FileSystem ..." (sadece iki adet 'new'!) satirinda
    // SONSUZA KADAR donuyordu. Init'i normal bir thread baglamina tasiyip
    // burada yalnizca sonucu bekliyoruz (qsort'ta ise yarayan ayni desen).
    {
        std::thread init_th([] { PsemuInitKytyGraphics(); });
        init_th.join();
    }
    finished.store(true, std::memory_order_release); // bekleyenleri serbest birak
    LOG_INFO("[KYTY-GFX] Vulkan HAZIR (instance/device/swapchain kuruldu).");
}

bool Dispatch(const std::string& nid, const std::string& name, CONTEXT* ctx) {
    std::string kyty_nid = nid.substr(0, nid.find('#')); // "...#A#B" -> "..."

    bool is_render =
        name.rfind("sceAgc", 0) == 0 || name.rfind("Graphics", 0) == 0 ||
        name.rfind("sceVideoOut", 0) == 0;

    // ========================================================================
    // PSEMU'NUN SAHIPLENDIGI ALANLAR - Kyty'ye DEVREDILMEZ
    // ------------------------------------------------------------------------
    // Kyty yonlendirmesi HLE zincirinden ONCE calisiyor; dolayisiyla Kyty
    // veritabaninda bulunan HER NID psemu'nun kendi handler'ini GOLGELER.
    // libKernel (365 fonksiyon) eklenince bu, bu oturumda olcup duzelttigimiz
    // her seyi geri alma riski tasiyor:
    //   - sceKernelPread/Lseek        (yoktu; varliklar BOS okunuyordu)
    //   - sceKernelOpen/Read/Close    (paylasilan fd tablosu; her dosya
    //                                  sifir okunuyordu)
    //   - sceKernelStat/Fstat         (120 bayt yerine 128 yazip yigin
    //                                  canary'sini eziyordu)
    //   - sceKernel*Sema              (zaman asiminda BASARILI donuyordu)
    //   - sceKernel*EventFlag         (govdesizdi; ses alt sistemi bunlarla
    //                                  kalkti)
    //   - sceKernelAllocate/MapDirectMemory (tembel commit; 13.3 -> 4.7 GB)
    // DURUM: libKernel su an KAYITLI DEGIL (denendi, geri alindi - Kyty'nin
    // libKernel'i RuntimeLinker/Elf64/ag yuzeyini de cekiyor ve o katman
    // psemu'nun loader'iyla cakisiyor). Dolayisiyla bu koruma SU AN ETKISIZ;
    // ilerideki bir denemede yanlislikla psemu'nun cekirdek HLE'sinin
    // gölgelenmemesi icin BILEREK burada birakildi.
    // Kacis kapisi: PSEMU_KYTY_KERNEL_ALL=1 -> koruma kapali.
    static const bool s_kernel_all = [] {
        const char* e = std::getenv("PSEMU_KYTY_KERNEL_ALL");
        return e != nullptr && e[0] == '1';
    }();
    auto psemu_owns = [](const std::string& n) {
        static const char* kOwned[] = {
            "sceKernelOpen", "sceKernelRead", "sceKernelPread", "sceKernelWrite",
            "sceKernelClose", "sceKernelLseek", "sceKernelStat", "sceKernelFstat",
            "sceKernelAllocateDirectMemory", "sceKernelMapDirectMemory",
            "sceKernelMapNamedDirectMemory", "sceKernelReleaseDirectMemory",
            "sceKernelGetDirectMemorySize", "sceKernelMmap", "sceKernelMunmap",
        };
        for (const char* o : kOwned) {
            if (n == o) return true;
        }
        // libC tarafi: psemu'nun SICAK YOLLARI ve bu oturumda olcup
        // duzelttigimiz fonksiyonlar. memcpy/memset/memmove/memcmp native PLT
        // dispatch'ten geciyor (GOT'a dogrudan host adresi yaziliyor); Kyty'ye
        // yonlendirmek her cagriyi VEH turuna sokar ve olculmus hizlanmayi
        // (FPS 5-7 -> 15) geri alir. printf ailesi ve _s ailesi de bizde
        // duzeltildi (sprintf_s bos donuyordu, strnstr hep 0 donuyordu).
        static const char* kOwnedLibc[] = {
            "memcpy", "memset", "memmove", "memcmp", "strlen", "strcmp", "strncmp",
            "strcpy", "strncpy", "strcat", "strstr", "strnstr", "strchr", "strrchr",
            "sprintf", "snprintf", "vsnprintf", "printf", "libc_printf", "puts",
            "fputs", "fflush", "sprintf_s", "vsprintf_s", "strcpy_s", "strncpy_s",
            "strncat_s", "memcpy_s", "memmove_s",
        };
        for (const char* o : kOwnedLibc) {
            if (n == o) return true;
        }
        // Aile bazinda: semafor, olay bayragi, olay kuyrugu, thread/pthread.
        return n.find("Sema") != std::string::npos ||
               n.find("EventFlag") != std::string::npos ||
               n.find("Equeue") != std::string::npos ||
               n.rfind("scePthread", 0) == 0;
    };

    if (!is_render && !kyty_nid.empty() && !(!s_kernel_all && psemu_owns(name)) &&
        PsemuKytyHasNid(kyty_nid.c_str())) {
        is_render = true;
    }

    if (!is_render) return false;

    EnsureKytyGraphicsInit();

    // Forward to Kyty's real graphics implementation (PM4 Command Buffer builder)
    if (!kyty_nid.empty() && PsemuKytyAgcCall(kyty_nid.c_str(), ctx)) {
        return true;
    }

    // ========================================================================
    // TANI: KYTY'DE OLMAYAN GRAFIK FONKSIYONLARI SESSIZCE 0 DONUYOR
    // ------------------------------------------------------------------------
    // Bu dala dusen her fonksiyon "basarili" gorunup RAX=0 donduruyor. Bir
    // KAYNAK OLUSTURMA cagrisi buraya duserse cagiran nullptr alir ve bunu
    // ancak cok sonra fark eder. Astro Bot tam boyle oluyor:
    //   Assertion failed: depthTarget != nullptr
    //   GfxRenderPipelineResourceManager.cpp:113
    // Ayni "sessizce hicbir sey yapma" deseni bu oturumda iki kez daha kok
    // neden cikti (eksik wmemcpy -> bos konusma metni; fputs'un akis
    // argumanini yok saymasi -> 0 baytlik kayit dosyalari).
    //
    // Her ISIM icin YALNIZCA BIR KEZ basiyoruz: liste sinirli ve tekrar
    // eden cagrilar logu bogardi.
    {
        static std::mutex              s_m;
        static std::set<std::string>   s_seen;
        std::lock_guard<std::mutex>    lk(s_m);
        if (s_seen.insert(name).second) {
            printf("[AGC-YOK] %s (NID %s) Kyty'de yok -> sessizce RAX=0\n",
                   name.c_str(), kyty_nid.c_str());
            fflush(stdout);
        }
    }
    ctx->Rax = 0;
    return true;
}

} // namespace Agc
