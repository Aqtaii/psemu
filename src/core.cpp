#include "core.h"
#include "agc.h"
#include "logger.h"
#include "syscalls.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <map>
#include <set>
#include <vector>
#include <cctype>
#include <cstdio>
#include <cerrno>   // strtoull/strtod'un ERANGE bildirimi icin
#include <cmath>    // isnan/isinf (fp_isfinite)
#include <mutex>
#include <memory>  // tembel tani stringstream'i icin unique_ptr
#include <deque>    // direct memory havuzu kilidi
#include <tlhelp32.h> // ThreadSamplerProc: surecteki tum thread'leri gezmek icin
#include "nids.h"
#include "game_profile.h"
#include "video.h"
#include "kernel/eventQueue.h"
#include "graphics/presentation/videoOut.h"
#include "libs/controller.h"
#include "libs/padData.h"
#include <immintrin.h>
#include <timeapi.h>  // timeBeginPeriod: Sleep granulerligi

extern "C" void PsemuMarkCpuModified(uint64_t vaddr, uint64_t size);
// Performans metrikleri (gpu/adapter/metrics.cpp) - pencere basliginda gosterilir.
extern "C" void PsemuMetricAddTlsFault();
extern "C" void PsemuMetricAddPltCall();
extern "C" void PsemuDumpPltTop();
extern "C" void PsemuMetricAddVehCycles(unsigned long long cycles);

// ========================================================
// SysV AMD64 va_list printf formatlayici
// ========================================================
// Oyun kendi loglarini vsnprintf/vfprintf ile formatliyor. va_list (SysV)
// bir __va_list_tag yapisidir: {gp_offset, fp_offset, overflow_arg_area,
// reg_save_area}. Argumanlari buradan cekip her donusumu native snprintf ile
// tek tek formatliyoruz (genislik/hassasiyet/bayraklar native'e birakilir).
// ========================================================
// GUVENLI BELLEK OKUMA (IsBadReadPtr YERINE)
// ========================================================
// ONEMLI: IsBadReadPtr/IsBadWritePtr icten SEH probe yapar. Bu kod VEH
// handler'inin ICINDEN calistigi ve VEH'imiz oncelik 1 ile kayitli oldugu
// icin probe'un urettigi exception once BIZE dusuyor -> IsBadReadPtr gecerli
// adresler icin bile hatali sonuc verebiliyor. (Oturumda daha once recursive-VEH
// olarak yakalanan hatanin ayni sinifi.) VirtualQuery exception uretmez.
// OLCUM: SafeReadable/SafeWritable bolge bolge VirtualQuery yapar. Misafir
// bellegi cok parcaliysa (bizim 64KB'lik otomatik commit'lerimiz ve tek tek
// RO->RW cevirdigimiz sayfalar araligi bolduğu icin) buyuk bir aralik yuzlerce
// sorgu demek olabilir. mem* fonksiyonlarinin cagri basina 250k-1.1M dongu
// yakmasinin sebebi gercek veri hacmi mi yoksa bu dogrulama mi - sayaclar
// PLT-TOP dokumunde raporlaniyor.
std::atomic<uint64_t> g_vq_calls{0};
std::atomic<uint64_t> g_memcmp_bytes{0}; // memcmp'e verilen toplam n
std::atomic<uint64_t> g_memcmp_max{0};   // gorulen en buyuk n
std::atomic<uint64_t> g_veh_nested{0};   // VEH'in kendi icinden tekrar girilme sayisi

static bool SafeReadable(const void* p, size_t n) {
    if (p == nullptr || n == 0) return false;
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(p);
    const uint8_t* end = cur + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        g_vq_calls.fetch_add(1, std::memory_order_relaxed);
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS) return false;
        if (mbi.Protect & PAGE_GUARD) return false;
        const uint8_t* region_end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) return false; // ilerleme yoksa cik
        cur = region_end;
    }
    return true;
}

// Null-terminated C string'in uzunlugunu guvenle olcer.
// ONEMLI: Karakter basina VirtualQuery yapmak yerine BOLGE BOLGE tarar.
// Bu hem cok daha hizlidir hem de yapay bir uzunluk siniri gerektirmez.
// (Onceki surumde strlen 1MB ile sinirliydi; 1.245.105 byte'lik data.js
// kesilip JSON parser "unexpected end of input" hatasi veriyordu.)
// Hedef aralik BASTAN SONA yazilabilir mi? Bolge bolge dogrular.
// IsBadWritePtr VEH icinde guvenilmez oldugu icin (kendi SEH probe'u bize
// dusuyor) VirtualQuery kullaniyoruz. Sadece ilk bayti kontrol etmek
// yetmiyordu: buyuk n degerlerinde commit edilmis bolgenin sonundan tasip
// direct-memory havuzunun rezerve ama commit edilmemis sayfasina yaziyor,
// WRITE violation uretiyorduk.
static bool SafeWritable(void* p, size_t n) {
    if (p == nullptr || n == 0) return false;
    uint8_t* cur       = reinterpret_cast<uint8_t*>(p);
    uint8_t* const end = cur + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        g_vq_calls.fetch_add(1, std::memory_order_relaxed);
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool writable = (prot == PAGE_READWRITE) || (prot == PAGE_WRITECOPY) ||
                              (prot == PAGE_EXECUTE_READWRITE) || (prot == PAGE_EXECUTE_WRITECOPY);
        if (!writable || (mbi.Protect & PAGE_GUARD)) return false;
        uint8_t* region_end = reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) return false; // ilerleme yoksa sonsuz dongu olmasin
        cur = region_end;
    }
    return true;
}

static size_t SafeStrlen(const char* p, size_t max_len = SIZE_MAX) {
    if (p == nullptr) return 0;
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(p);
    size_t len = 0;
    while (len < max_len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State != MEM_COMMIT) break;
        if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) break;

        const uint8_t* region_end =
            reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) break; // ilerleme yoksa cik

        for (; cur < region_end && len < max_len; ++cur, ++len) {
            if (*cur == 0) return len;
        }
    }
    return len;
}

// Null-terminated C string'i guvenle okur (sayfa sinirlarina saygili).
static std::string SafeReadCString(const char* p, size_t max_len = SIZE_MAX) {
    if (p == nullptr) return std::string();
    size_t n = SafeStrlen(p, max_len);
    if (n == 0) return std::string();
    return std::string(p, p + n);
}

static std::string FormatSysVPrintf(const char* fmt, uint8_t* va) {
    std::string out;
    if (!SafeReadable(fmt, 1)) return out;
    // va_list gecersizse en azindan format string'in kendisini goster
    if (va == nullptr || !SafeReadable(va, 24)) { return SafeReadCString(fmt); }

    uint32_t gp_offset = *reinterpret_cast<uint32_t*>(va + 0);
    uint32_t fp_offset = *reinterpret_cast<uint32_t*>(va + 4);
    uint8_t* overflow  = *reinterpret_cast<uint8_t**>(va + 8);
    uint8_t* reg_save  = *reinterpret_cast<uint8_t**>(va + 16);

    auto nextGP = [&]() -> uint64_t {
        uint64_t v = 0;
        if (gp_offset < 48 && SafeReadable(reg_save + gp_offset, 8)) {
            v = *reinterpret_cast<uint64_t*>(reg_save + gp_offset); gp_offset += 8;
        } else if (SafeReadable(overflow, 8)) {
            v = *reinterpret_cast<uint64_t*>(overflow); overflow += 8;
        }
        return v;
    };
    auto nextFP = [&]() -> double {
        double v = 0;
        if (fp_offset < 176 && SafeReadable(reg_save + fp_offset, 8)) {
            v = *reinterpret_cast<double*>(reg_save + fp_offset); fp_offset += 16;
        } else if (SafeReadable(overflow, 8)) {
            v = *reinterpret_cast<double*>(overflow); overflow += 8;
        }
        return v;
    };

    char tmp[1024];
    for (const char* p = fmt; *p; ) {
        if (*p != '%') { out += *p++; continue; }
        const char* start = p++;             // '%'
        if (*p == '%') { out += '%'; p++; continue; }
        while (*p && strchr("-+ #0", *p)) p++;                 // bayraklar
        while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == '*')) {
            if (*p == '*') (void)nextGP();                     // dinamik genislik/hassasiyet
            p++;
        }
        int longness = 0;                                     // uzunluk belirteci
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') {
            if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't') longness++;
            p++;
        }
        char conv = *p ? *p++ : 0;
        std::string spec(start, p - start);                   // tam "%...conv"
        tmp[0] = 0;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                uint64_t a = nextGP();
                // 64-bit'e normalize et (Windows'ta 'l' 32-bit oldugundan spec'i
                // yeniden kurup ll kullan)
                std::string s2 = spec.substr(0, spec.size() - 1);
                // uzunluk harflerini temizle
                std::string cleaned; for (char ch : s2) if (!strchr("lhzjtL", ch)) cleaned += ch;
                cleaned += "ll"; cleaned += conv;
                snprintf(tmp, sizeof(tmp), cleaned.c_str(), a);
                break;
            }
            case 'p': { snprintf(tmp, sizeof(tmp), "0x%llx", nextGP()); break; }
            case 'c': { snprintf(tmp, sizeof(tmp), "%c", (int)nextGP()); break; }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                snprintf(tmp, sizeof(tmp), spec.c_str(), nextFP());
                break;
            }
            case 's': {
                const char* sp = reinterpret_cast<const char*>(nextGP());
                if (SafeReadable(sp, 1)) { out += SafeReadCString(sp); }
                else { out += "(null)"; }
                tmp[0] = 0;
                break;
            }
            default: { out += spec; tmp[0] = 0; break; }
        }
        out += tmp;
        if (out.size() > 8192) break; // guvenlik siniri
    }
    return out;
}

// printf/fprintf gibi DOGRUDAN degisken argumanli (va_list almayan)
// fonksiyonlar icin CONTEXT register'larindan SysV va_list sentezler.
// named_gp = degisken olmayan GP arguman sayisi (printf=1 [fmt], fprintf=2).
static std::string FormatVariadicFromCtx(const char* fmt, PCONTEXT ctx, int named_gp) {
    uint8_t reg_save[176];
    memset(reg_save, 0, sizeof(reg_save));

    // SysV reg_save_area: 0..47 = RDI,RSI,RDX,RCX,R8,R9 ; 48..175 = XMM0-7
    uint64_t gp[6] = { ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx->Rcx, ctx->R8, ctx->R9 };
    memcpy(reg_save, gp, sizeof(gp));
    const M128A* xmm = &ctx->Xmm0;
    for (int i = 0; i < 8; i++) memcpy(reg_save + 48 + i * 16, &xmm[i], 16);

    uint8_t va[24];
    *reinterpret_cast<uint32_t*>(va + 0) = static_cast<uint32_t>(named_gp * 8); // gp_offset
    *reinterpret_cast<uint32_t*>(va + 4) = 48;                                   // fp_offset
    // Stack argumanlari: PLT'de yakaladigimiz icin RSP donus adresini gosteriyor
    *reinterpret_cast<uint8_t**>(va + 8)  = reinterpret_cast<uint8_t*>(ctx->Rsp + 8);
    *reinterpret_cast<uint8_t**>(va + 16) = reg_save;

    return FormatSysVPrintf(fmt, va);
}

// ========================================================
// VFS: guest "/app0/..." -> host oyun dizini
// ========================================================
std::string g_game_root = ".";

// Bizim actigimiz FILE* handle'lari (oyun gecersiz bir pointer verirse
// host CRT'yi cokertmemek icin dogrulama listesi).
static std::set<FILE*> g_open_files;
// Tani icin: hangi FILE* hangi dosyaya ait
static std::map<FILE*, std::string> g_open_names;
// VFS thread-guvenligi: oyun kaynaklari ARKA PLANDA cok thread'le yukluyor
// (fadein_white-sheet0/sheet1 esZAMANLI aciliyor). g_open_files/g_open_names
// std::set/std::map'leri kilitsiz eszamanli erisimde BOZULUYOR: bir thread'in
// fopen'daki insert'i, digerinin fread'indeki .count(f) agac gezinmesini
// bozup 0 donduruyor -> known=false -> got=0 -> KISA OKUMA -> oyun
// "Failed loading image" deyip exit(1) ediyordu. Tum VFS islemlerini
// serilestiriyoruz (yukleme darbogaz degil; dogruluk > hiz).
static std::mutex g_vfs_mutex;

// Guest yolunu host yoluna cevirir. PS4/PS5'te "/app0/" oyunun kendi
// klasorudur; diger mutlak yollari da ayni koke baglariz.
// Kayit verisi kok klasoru. Oyunun /saveDataN/... yollari BURAYA eslenir.
// Oyun dizinini kirletmemek ve gercek kayit destegi verebilmek icin ayri tutulur.
// TITLE ID ile ayrilir ("savedata/PPSA02929"): iki oyun birbirinin kaydini
// EZMESIN. Profil cozulemezse duz "savedata"ya duser (bkz. game_profile.cpp).
static std::string SaveDataRoot() {
    return Game::Current().savedata_root;
}

// Verilen HOST yolunun ust dizinlerini olusturur (yoksa). fopen("wb") ancak
// dizin varsa basarili olur; oyun kayit dosyasini olusturamayinca
// "/saveData0/-saveindex" acilamiyor ve oyun o asamada takiliyordu.
static void EnsureParentDirs(const std::string& host_path) {
    std::string acc;
    for (size_t i = 0; i < host_path.size(); i++) {
        const char c = host_path[i];
        if (c == '/' || c == '\\') {
            if (!acc.empty() && acc.back() != ':') {
                CreateDirectoryA(acc.c_str(), nullptr); // varsa hata vermez
            }
        }
        acc.push_back(c);
    }
}

static std::string TranslateGuestPath(const std::string& guest) {
    if (guest.empty()) return guest;
    if (guest.rfind("/app0/", 0) == 0)  return g_game_root + "/" + guest.substr(6);
    if (guest == "/app0")               return g_game_root;
    if (guest.rfind("/hostapp/", 0) == 0) return g_game_root + "/" + guest.substr(9);
    // Kayit verisi mount'lari (/savedata0, /saveData0, ...) -> savedata/...
    if (guest.size() > 9) {
        std::string low = guest.substr(0, 9);
        for (auto& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (low == "/savedata") {
            return SaveDataRoot() + guest;
        }
    }
    if (guest[0] == '/')                return g_game_root + guest;  // diger mutlak yollar
    return guest;                                                     // goreli yol
}

// ========================================================
// LOG GURULTU FILTRESI
// ========================================================
// Oyun artik gercekten CALISIYOR (ses dongusu, worker thread'ler, VFS parse).
// Her PLT cagrisini loglamak saniyede binlerce satir uretip ilerlemeyi
// gorunmez kiliyor. Her fonksiyonu ilk N kez logla, sonra sustur; boylece
// YENI/nadir olaylar (VideoOut, cokmeler, oyun mesajlari) one cikar.
static std::map<uint32_t, uint64_t> g_plt_call_counts;
static const uint64_t kPltLogLimit = 8;

// Bu PLT icin log basilmali mi? (ilk kez limiti asinca bir kez uyari verir)
static bool ShouldLogPlt(uint32_t plt_index, const std::string& label) {
    uint64_t n = ++g_plt_call_counts[plt_index];
    if (n <= kPltLogLimit) return true;
    if (n == kPltLogLimit + 1) {
        std::stringstream ss;
        ss << "[LOG-FILTRE] " << label << " (PLT#" << plt_index << ") " << kPltLogLimit
           << " kez loglandi, bundan sonra susturuluyor (cagrilar calismaya devam ediyor).";
        LOG_INFO(ss.str());
    }
    return false;
}

// ========================================================
// GNM KOMUT TAMPONU (CommandBuffer) YARDIMCISI
// ========================================================
// Oyun CommandBuffer'i KENDISI ayirir ve bize pointer'ini verir; Dcb*/Cb*
// fonksiyonlari oraya PM4 paketi yazip yazdiklari yerin adresini dondurur.
// KytyPS5 (src/libs/agc.cpp) yapisi:
//   0x00 bottom  0x08 top  0x10 cursor_up  0x18 cursor_down
//   0x20 callback  0x28 user_data  0x30 reserved_dw
// AllocateDW: ret = cursor_up; cursor_up += size_dw; return ret;
static uint32_t* CbAllocateDW(uint64_t buf_addr, uint32_t size_dw) {
    if (buf_addr == 0 || size_dw == 0) return nullptr;
    uint8_t* b = reinterpret_cast<uint8_t*>(buf_addr);
    // Cursor alanina YAZACAGIZ (b+0x10), okuma kontrolu yetmez.
    if (!SafeWritable(b, 0x20)) return nullptr;

    uint32_t** cursor_up_p = reinterpret_cast<uint32_t**>(b + 0x10);
    uint32_t*  cursor_down = *reinterpret_cast<uint32_t**>(b + 0x18);
    uint32_t*  cur         = *cursor_up_p;

    // Doner donmez cagiran buraya PM4 paketi YAZIYOR; tum araligin
    // yazilabilir oldugunu dogrula (commit edilmemis direct-memory
    // sayfasina tasip access violation uretiyorduk).
    if (cur == nullptr || !SafeWritable(cur, static_cast<size_t>(size_dw) * 4)) return nullptr;
    // Tampon tasmasini onle (cursor_down yukari dogru sinirdir)
    if (cursor_down != nullptr && cur + size_dw > cursor_down) return nullptr;

    *cursor_up_p = cur + size_dw;
    return cur;
}

// PM4 paket basligi (KytyPS5 pm4.h KYTY_PM4 makrosu ile ayni)
static inline uint32_t Pm4Header(uint32_t len_dw, uint32_t opcode) {
    return 0xC0000000u | (((len_dw - 2u) & 0x3fffu) << 16u) | ((opcode & 0xffu) << 8u);
}
static const uint32_t kPm4_IT_SET_SH_REG = 0x76;

// ========================================================
// ZAMAN KAYNAGI
// ========================================================
// Oyun clock_gettime/gettimeofday ile sure olcuyor. Stub'lar cikti
// struct'ini doldurmayinca oyun baslatilmamis bellekten cop okuyup
// "-4294967296.000001" gibi sacma sureler hesapliyordu.
// QueryPerformanceCounter ile mikrosaniye altinda cozunurluk veriyoruz.
static LARGE_INTEGER g_qpc_freq  = {};
static LARGE_INTEGER g_qpc_start = {};

static void TimeInit() {
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_start);
}

// Process baslangicindan itibaren gecen nanosaniye (monotonik)
static uint64_t MonotonicNs() {
    if (g_qpc_freq.QuadPart == 0) return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long ticks = now.QuadPart - g_qpc_start.QuadPart;
    // Tasmayi onlemek icin saniye ve kalan ayri hesaplanir
    long long secs = ticks / g_qpc_freq.QuadPart;
    long long rem  = ticks % g_qpc_freq.QuadPart;
    return static_cast<uint64_t>(secs) * 1000000000ull +
           static_cast<uint64_t>(rem * 1000000000ll / g_qpc_freq.QuadPart);
}

// Unix epoch'tan itibaren gecen nanosaniye (duvar saati)
static uint64_t RealtimeNs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // FILETIME: 1601'den beri 100ns birim. Unix epoch'a kaydir.
    const uint64_t kEpochDiff100ns = 116444736000000000ull;
    if (t < kEpochDiff100ns) return 0;
    return (t - kEpochDiff100ns) * 100ull;
}

// Guest'in errno'su. BSD libc'de __error() bunun ADRESINI dondurur.
// nlohmann'in sayi ayristiricisi cagri oncesi 0'a cekip sonrasinda
// ERANGE kontrol ettigi icin strtoull/strtoll/strtod ile paylasilmali.
// ========================================================
// TAHSIS BOYUTU KAYDI (realloc'un dogru kopyalamasi icin)
// ========================================================
// realloc, eski bloktan YENI boyut kadar kopyalamaya calisiyordu; blok
// buyutulurken SafeReadable(old, yeni_boyut) eski blogun sonunu asip
// basarisiz olunca HIC kopyalamiyor ve sifirlanmis blok donduruyordu ->
// tum eski icerik KAYBOLUYORDU. Gorulen sonuc: dil kodu "us" -> "" ,
// ardindan "KEY NOT FOUND: music_atten/master_atten" ve ana menu
// metinlerinin bos kalmasi. Cozum: istenen boyutu kaydet, realloc
// min(eski, yeni) kadar kopyalasin.
// NOT: free() kasitli olarak no-op oldugu icin kayitlar bayatlamaz.
static std::mutex             g_alloc_size_mutex;
static std::map<void*, size_t> g_alloc_sizes;

static void RegisterAllocSize(void* p, size_t requested) {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> lk(g_alloc_size_mutex);
    g_alloc_sizes[p] = requested;
}

static size_t LookupAllocSize(void* p) {
    if (p == nullptr) return 0;
    std::lock_guard<std::mutex> lk(g_alloc_size_mutex);
    auto it = g_alloc_sizes.find(p);
    return (it == g_alloc_sizes.end()) ? 0 : it->second;
}

// ========================================================
// NID ONEK INDEKSI (son ek farkliliklarini tolere eder)
// ========================================================
// nids.h tablosu NID'leri "<11 karakter NID>#<kutuphane>#<modul>" olarak
// tutuyor. Son ek oyuna gore DEGISIR; fonksiyonu tekil belirleyen sey
// 11 karakterlik NID'in kendisidir. Tam eslesme aranirsa son eki farkli
// olan fonksiyonlar cozulemez ve default stub'a (RAX=0) duser.
// Gercek ornek: QOQtbeDqsT4#N#O = sceAudioOutOutput (tabloda #T#T vardi).
// Bu indeks bir kez kurulur, sonra O(1) onek aramasi saglar.
static const std::unordered_map<std::string, const std::string*>& NidPrefixIndex() {
    static const std::unordered_map<std::string, const std::string*> idx = [] {
        std::unordered_map<std::string, const std::string*> m;
        for (const auto& kv : g_nid_to_name) {
            if (kv.first.size() >= 11) {
                m.emplace(kv.first.substr(0, 11), &kv.second);
            }
        }
        return m;
    }();
    return idx;
}

// ========================================================
// GUEST KARSILASTIRICI KOPRUSU (qsort/bsearch icin)
// ========================================================
// Guest kod System V AMD64 ABI kullanir (args RDI/RSI/...), host ise Windows
// x64 ABI (RCX/RDX/...). clang-cl'in sysv_abi niteligi ile guest fonksiyon
// isaretcisini dogru ABI'yle cagiriyoruz. thread_local: her thread kendi
// karsilastiricisini tasir.
using GuestCmpFn = int(__attribute__((sysv_abi)) *)(const void*, const void*);
static thread_local GuestCmpFn t_guest_cmp = nullptr;
static int HostCmpBridge(const void* a, const void* b) {
    return (t_guest_cmp != nullptr) ? t_guest_cmp(a, b) : 0;
}

// Siralamayi VEH handler'inin ICINDE yapmak kilitlenmeye yol aciyor (guest
// karsilastiricisi kendi PLT fault'larini uretir -> ic ice exception). Bu yuzden
// isi AYRI BIR THREAD'e verip bekliyoruz: guest kodu normal bir thread baglaminda
// calisir, faulting thread yalnizca sonucu bekler.
struct GuestSortJob {
    void*      base;
    size_t     nmemb;
    size_t     size;
    GuestCmpFn cmp;
};
static uint64_t GetThreadTlsBase(); // asagida tanimli (TLS hizli yolu)

static DWORD WINAPI GuestSortThread(LPVOID param) {
    // Misafir karsilastiricisini cagiracagiz: bu thread'in TLS blogu ve TEB
    // slotu hazir olmali (yamali fs: komutlari artik fault uretmiyor).
    GetThreadTlsBase();
    auto* j     = static_cast<GuestSortJob*>(param);
    t_guest_cmp = j->cmp;
    std::qsort(j->base, j->nmemb, j->size, HostCmpBridge);
    t_guest_cmp = nullptr;
    return 0;
}

static thread_local int g_guest_errno = 0;
static const int kGuestERANGE = 34; // FreeBSD ERANGE

// Parse SADAKAT sayaclari. data.js'te 153913 tamsayi ve 12795 float var;
// bu sayaclar tutmuyorsa ayristirma dosyadan SAPIYOR demektir.
static volatile long g_n_strtoint = 0;
static volatile long g_n_strtod   = 0;

// ========================================================
// DIRECT MEMORY HAVUZU
// ========================================================
// PS5'te sceKernelAllocateDirectMemory bir FIZIKSEL adres verir ve
// sceKernelMapDirectMemory onu sanal adrese esler. Ayni fiziksel adres
// tekrar eslenirse AYNI bellek gorunmelidir.
// Onceki surum direct_memory_start (R8) parametresini yok sayip her
// cagrida taze VirtualAlloc yapiyordu; oyun bir eslemede kurdugu hash
// tablosunu digerinden sifir olarak okuyup RVA 0x105495'te cokuyordu.
// Cozum: tek buyuk rezervasyon, fiziksel adres = havuz icindeki offset.
// Boylece ayni phys her zaman ayni belleye dusuyor.
static uint8_t*        g_dmem_base = nullptr;
static std::mutex      g_dmem_mutex;
// PS5'te 16 GB birlesik bellek var ve Astro Bot fiziksel adres alaninda
// hizla yukari cikiyor: olculdu, T+48'de tahsis toplami 10349 MB'a ulasti
// (phys=0x282d00000). Rezerv 4 GB iken bu esigi asan her esleme yedek yola
// dusuyordu: "phys ile iliskisiz TAZE VirtualAlloc". Bu, dosyadaki
// "ayni phys -> ayni sanal adres" degismezini kiriyor ve daha kotusu, AGC
// modulu descriptor'lardaki FIZIKSEL adresi "CPU = g_dmem_base_addr + phys"
// ile ceviriyor - yani GPU o tamponlari HIC goremiyordu.
// Rezervasyon sadece adres alanidir (MEM_RESERVE), fiziksel bellek
// tuketmez; ihtiyac halinde commit ediliyor.
static const uint64_t  kDmemSize   = 0x400000000ULL; // 16 GB adres alani rezervi

// AGC (GPU) modulunun descriptor'lardaki FIZIKSEL adresleri CPU'ya cevirebilmesi
// icin havuz tabanini disari veriyoruz (CPU = g_dmem_base_addr + phys).
uint64_t g_dmem_base_addr = 0;

// mspace tutamaci -> kapasite. sceLibcMspaceCreate'in 3. argumani, daha
// sonra sceLibcMspaceMallocStats ile oyuna geri bildirilir.
struct MspaceInfo {
    uint64_t base = 0;   // sceLibcMspaceCreate'e verilen bellek
    uint64_t cap  = 0;   // kapasite
    uint64_t used = 0;   // bump ofseti
};
static std::mutex g_mspace_mtx;
static std::map<uint64_t, MspaceInfo> g_mspace;

static void MspaceRemember(uint64_t handle, uint64_t base, uint64_t cap) {
    std::lock_guard<std::mutex> lock(g_mspace_mtx);
    // AYNI TABAN iki kez kaydedilirse ikincisini bolgesiz birak. Astro Bot
    // ayni bellek uzerine ic ice mspace kuruyor (olculdu: base=0x...340000
    // hem cap=0xb800000 hem cap=0x200000 ile). Ikisine de ayni bolgeden
    // dagitsaydik birbirlerinin uzerine yazarlardi; bu durumda ikincisi
    // guvenli host yigin yoluna duser.
    for (const auto& kv : g_mspace) {
        if (kv.second.base == base && base != 0) {
            g_mspace[handle] = MspaceInfo{0, cap, 0};
            return;
        }
    }
    g_mspace[handle] = MspaceInfo{base, cap, 0};
}

static uint64_t MspaceCapacity(uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_mspace_mtx);
    auto it = g_mspace.find(handle);
    // Bilinmeyen tutamac icin comert bir varsayilan: "yer var" demek,
    // "yer yok" demekten cok daha guvenli (oyun yoksa hic denemiyor).
    return (it != g_mspace.end() && it->second.cap != 0) ? it->second.cap
                                                        : (256ull * 1024 * 1024);
}

static uint64_t MspaceUsed(uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_mspace_mtx);
    auto it = g_mspace.find(handle);
    return (it != g_mspace.end()) ? it->second.used : 0;
}

// Tutamac GERCEK bir bellek bolgesi uzerine kurulduysa tahsisi O BOLGENIN
// ICINDEN yap. Host yigininden vermek yanlisti: oyun bu isaretcileri GPU'ya
// veriyor ve havuz araliginda olmalarini bekliyor. Bolge yoksa 0 don
// (cagiran host yigin yoluna duser).
static void* MspaceBumpAlloc(uint64_t handle, size_t n, size_t align) {
    if (align < 16) align = 16;
    std::lock_guard<std::mutex> lock(g_mspace_mtx);
    auto it = g_mspace.find(handle);
    if (it == g_mspace.end() || it->second.base == 0 || it->second.cap == 0) {
        return nullptr;
    }
    MspaceInfo& m = it->second;
    uint64_t p = (m.base + m.used + align - 1) & ~(uint64_t)(align - 1);
    uint64_t end = p + n;
    if (end > m.base + m.cap) return nullptr; // havuz doldu
    m.used = end - m.base;
    return reinterpret_cast<void*>(p);
}

static uint8_t* DmemBase() {
    std::lock_guard<std::mutex> lock(g_dmem_mutex);
    if (g_dmem_base == nullptr) {
        g_dmem_base = reinterpret_cast<uint8_t*>(
            VirtualAlloc(nullptr, kDmemSize, MEM_RESERVE, PAGE_NOACCESS));
        if (g_dmem_base) {
            // Ilk 64 MB'yi hemen commit et (anlik baslangic).
            // Geri kalan 4 GB bellek ihtiyac duyuldugunda On-Demand Committer
            // tarafindan otomatik commit edilir.
            const size_t kInitCommit = 64ULL * 1024 * 1024;
            VirtualAlloc(g_dmem_base, kInitCommit, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        }
        g_dmem_base_addr = reinterpret_cast<uint64_t>(g_dmem_base);
    }
    return g_dmem_base;
}

// ========================================================
// PTHREAD MUTEX / CONDITION VARIABLE
// ========================================================
// Bunlar implement edilmemisti; hepsi genel stub'a dusup RAX=0 donuyordu.
// Sonuclari:
//   - Mutex'ler no-op -> karsilikli dislama YOK -> veri yarislari
//   - CondTimedwait 0 donuyor = "sinyallendi" -> is parcacigi havuzu
//     kuyruk BOSKEN gorev almis saniyor, cop nesne uzerinden sanal cagri
//     yapiyordu (RVA 0x2c06a0: call [rax] -> veri adresine sicrama).
// ABI KytyPS5 src/kernel/pthread.cpp'den dogrulandi:
//   PthreadMutexInit(PthreadMutex*, const PthreadMutexattr*)
//   PthreadMutexLock/Unlock(PthreadMutex*)
//   PthreadCondInit(PthreadCond*, const PthreadCondattr*)
//   PthreadCondTimedwait(PthreadCond*, PthreadMutex*, KernelUseconds)
// Guest tarafinda her ikisi de POINTER boyutunda opak tutamac; Init
// cagrisi *slot'a tutamaci yazar. Statik baslatilan (Init cagrilmadan
// kullanilan) nesneler icin tembel kurulum yapiyoruz.
struct GuestMutex { CRITICAL_SECTION cs; };
struct GuestCond  { CONDITION_VARIABLE cv; };

static std::mutex          g_sync_create_mutex;
static std::set<uint64_t>  g_known_mutexes;
static std::set<uint64_t>  g_known_conds;

// SICAK YOL ONBELLEGI (thread'e ozel, kilitsiz).
// Olcum: oyun menude saniyede ~85.000 kez mutex kilitleyip aciyor ve eski hal
// HER cagride (a) SafeWritable -> VirtualQuery, (b) TUM threadleri seri hale
// getiren tek bir global std::mutex, (c) std::set agac aramasi yapiyordu.
// Olculen maliyet: cagri basina ~83.000 CPU dongusu (~18 us) -> tek basina
// kare suresinin buyuk kismi. Ayni mutex arka arkaya kilitlenip acildigi icin
// kucuk bir dogrudan-eslemeli onbellek neredeyse %100 isabet ediyor.
namespace {
struct MtxCacheEnt {
    uint64_t*   slot = nullptr;
    GuestMutex* m    = nullptr;
    uint64_t    val  = 0; // onbellege alindigi andaki *slot degeri
};
} // namespace
static thread_local MtxCacheEnt t_mtx_cache[16];

static GuestMutex* GetOrCreateMutex(uint64_t* slot) {
    if (slot == nullptr) return nullptr;

    // Isabet sarti: ayni slot adresi VE slot icerigi degismemis olsun. Icerigi
    // okumak guvenli, cunku bu adresi onbellege almadan once SafeWritable ile
    // dogruladik; oyun slotu degistirirse esitlik tutmaz ve yavas yola duseriz.
    MtxCacheEnt& e = t_mtx_cache[(reinterpret_cast<uintptr_t>(slot) >> 4) & 15u];
    if (e.slot == slot && e.m != nullptr && *slot == e.val) {
        return e.m;
    }

    if (!SafeWritable(slot, sizeof(uint64_t))) return nullptr;
    GuestMutex* result = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_sync_create_mutex);
        uint64_t h = *slot;
        if (h != 0 && g_known_mutexes.count(h) != 0) {
            result = reinterpret_cast<GuestMutex*>(h);
        } else {
            GuestMutex* m = new GuestMutex();
            InitializeCriticalSection(&m->cs);
            *slot = reinterpret_cast<uint64_t>(m);
            g_known_mutexes.insert(*slot);
            result = m;
        }
    }
    e.slot = slot;
    e.m    = result;
    e.val  = *slot;
    return result;
}

// ============================================================================
// NATIVE PLT: exception'siz dogrudan cagri
// ----------------------------------------------------------------------------
// Simdiye kadar HER HLE cagrisi bir Windows exception turuydu: PLT stub'i
// haritalanmamis bir "sentinel" adrese jmp eder, CPU access violation uretir,
// VEH devreye girer, dispatch eder ve RET'i elle simule ederdi. Olculen yuk:
// kare basina ~21.000 cagri (130k/sn / 6 FPS) -> 167 ms'lik karenin neredeyse
// tamami.
//
// Oysa PLT stub'i "jmp [GOT]" yapiyor. GOT slotuna sentinel yerine SysV ABI'li
// NATIVE bir fonksiyonun adresini yazarsak misafir dogrudan oraya atlar ve
// fonksiyonun kendi "ret"i cagirana geri doner - exception hic olusmaz.
// clang'in __attribute__((sysv_abi))'si sayesinde assembly'e gerek yok.
//
// Buradaki fonksiyonlar VEH yolundaki karsiliklariyla AYNI davranisi
// uretmelidir. SafeReadable/SafeWritable dogrulamalari bilerek YOK: misafir
// gecerli isaretciler veriyor, commit edilmemis sayfa olursa zaten VEH'in
// otomatik commit yolu devreye giriyor (ve o yol bu fonksiyonlarin icinden
// tetiklendiginde de calisir).
//
// Kill switch: PSEMU_NATIVE_PLT=0 -> hepsi eski VEH yoluna doner.
// ============================================================================
#define PSEMU_SYSV __attribute__((sysv_abi))

static PSEMU_SYSV void* NativeMemcpy(void* d, const void* s, size_t n) {
    return (d && s && n) ? memcpy(d, s, n) : d;
}
static PSEMU_SYSV void* NativeMemset(void* d, int c, size_t n) {
    return (d && n) ? memset(d, c, n) : d;
}
static PSEMU_SYSV void* NativeMemmove(void* d, const void* s, size_t n) {
    return (d && s && n) ? memmove(d, s, n) : d;
}
static PSEMU_SYSV int NativeMemcmp(const void* a, const void* b, size_t n) {
    return (a && b && n) ? memcmp(a, b, n) : 0;
}
static PSEMU_SYSV size_t NativeStrlen(const char* s) { return s ? strlen(s) : 0; }

// VEH'teki "scePthreadGetthreadid" dali ile BIREBIR ayni deger; tek fark
// exception turu olmadan calismasi.
static PSEMU_SYSV uint64_t NativeGetThreadId() { return GetCurrentThreadId(); }

static PSEMU_SYSV int NativeMutexLock(uint64_t* slot) {
    GuestMutex* m = GetOrCreateMutex(slot);
    if (m != nullptr) EnterCriticalSection(&m->cs);
    return 0;
}
static PSEMU_SYSV int NativeMutexUnlock(uint64_t* slot) {
    GuestMutex* m = GetOrCreateMutex(slot);
    if (m != nullptr) LeaveCriticalSection(&m->cs);
    return 0;
}
// sceKernelWaitSema henuz implemente degil; VEH yolu da RAX=0 donuyordu.
// Davranis birebir ayni, yalnizca exception maliyeti yok.
static PSEMU_SYSV int NativeRet0() { return 0; }
static PSEMU_SYSV int* NativeErrno() { return &g_guest_errno; }
static PSEMU_SYSV int NativeStrcmp(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return 0;
    return strcmp(a, b);
}
static PSEMU_SYSV int NativeUsleep(uint64_t us) {
    const DWORD ms = static_cast<DWORD>(us / 1000);
    Sleep(ms != 0 ? ms : 1);
    return 0;
}
// libc_char_table: bugun HIC implemente degil, zincirin sonundaki varsayilan
// stub'a dusup 0 donuyor. Burada da BILEREK 0 donduruyoruz - bu adim yalnizca
// exception maliyetini kaldirmali, davranisi degil. Oyun bunu saniyede ~3.700
// kez cagiriyor, yani tek basina kalan yukun buyuk kismi.
// AYRI KONU: NULL dondurmek muhtemelen yanlis (gercek bir ctype tablosu
// donmeli, bkz. _Getpctype icin kurdugumuz 257 girdili tablo). Once dogru
// tablonun duzenini dogrulamak gerek; tahminle doldurmak yeni hata uretir.
static PSEMU_SYSV void* NativeCharTable() { return nullptr; }

// ============================================================================
// VERI SEMBOLU olarak import edilen FONKSIYONLAR
// ----------------------------------------------------------------------------
// Oyun bazi fonksiyonlarin ADRESINI veri olarak import eder (R_X86_64_64 /
// GLOB_DAT). En onemlisi __cxa_pure_virtual: derleyici bunu HER soyut sinifin
// vtable'ina yazar - Astro Bot'ta 1176 slot.
//
// psemu bu sembolleri cozemedigi icin her biri icin SIFIRLANMIS ve
// CALISTIRILAMAZ (PAGE_READWRITE) bir sayfa ayirip adresini yaziyordu. Sonuc:
// bir saf sanal cagri yapildiginda veri sayfasina atlaniyor -> gecersiz komut
// / vahsi dallanma. Astro Bot'taki "komut ortasina dusen, kosudan kosuya
// degisen" cokmelerin profili tam buydu.
//
// Burada bu semboller icin GERCEK fonksiyon adresi donduruyoruz.
static PSEMU_SYSV void NativePureVirtual() {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) < 8) {
        LOG_ERROR("[PURE-VIRTUAL] Oyun saf sanal bir fonksiyonu cagirdi (nesne henuz "
                  "kurulmamis ya da yikilmis olabilir). Cokmemek icin donuluyor.");
    }
}
static PSEMU_SYSV void* NativeMalloc(size_t n) {
    const size_t sz = n ? (n + 65536) : 65536;
    void*        p  = _aligned_malloc(sz, 16);
    if (p != nullptr) {
        memset(p, 0, sz);
        RegisterAllocSize(p, n);
    }
    return p;
}
static PSEMU_SYSV void NativeFree(void* p) {
    if (p != nullptr) _aligned_free(p);
}
static PSEMU_SYSV int NativePersonality() { return 0; }

// Veri sembolu olarak import edilen bir FONKSIYONUN native adresi (yoksa
// nullptr => eski "bos hucre" davranisi). loader.cpp relocation sirasinda
// cagirir; parametre ham NID dizesidir.
extern "C" void* PsemuNativeDataSymbol(const char* raw_nid) {
    if (raw_nid == nullptr) return nullptr;
    std::string nid(raw_nid);
    const std::string* rn = nullptr;
    auto exact = g_nid_to_name.find(nid);
    if (exact != g_nid_to_name.end()) {
        rn = &exact->second;
    } else if (nid.size() >= 11) {
        const auto& pidx = NidPrefixIndex();
        auto pit = pidx.find(nid.substr(0, 11));
        if (pit != pidx.end()) rn = pit->second;
    }
    if (rn == nullptr) return nullptr;

    const std::string& n = *rn;
    if (n == "__cxa_pure_virtual")   return reinterpret_cast<void*>(&NativePureVirtual);
    if (n == "__gxx_personality_v0") return reinterpret_cast<void*>(&NativePersonality);
    if (n == "malloc")               return reinterpret_cast<void*>(&NativeMalloc);
    if (n == "free")                 return reinterpret_cast<void*>(&NativeFree);
    if (n == "memcpy")               return reinterpret_cast<void*>(&NativeMemcpy);
    if (n == "memset")               return reinterpret_cast<void*>(&NativeMemset);
    return nullptr;
}

// Cozulemeyen bir C++ VTABLE sembolu (_ZTV...) icin hucreyi GUVENLI doldurur.
// Vtable bir isaretci dizisidir; sifirla doldurulursa oyunun o siniftan bir
// nesnede yaptigi HER sanal cagri adres 0'a (veya cop veriye) atlar. Bunun
// yerine tum slotlari "hicbir sey yapip donen" native fonksiyona baglayip
// vahsi dallanmayi engelliyoruz.
// true donerse cagiran hucreyi sifirlamamali.
extern "C" bool PsemuFillVtableCell(void* cell, size_t bytes, const char* raw_nid) {
    if (cell == nullptr || raw_nid == nullptr) return false;

    std::string nid(raw_nid);
    const std::string* rn = nullptr;
    auto exact = g_nid_to_name.find(nid);
    if (exact != g_nid_to_name.end()) {
        rn = &exact->second;
    } else if (nid.size() >= 11) {
        const auto& pidx = NidPrefixIndex();
        auto pit = pidx.find(nid.substr(0, 11));
        if (pit != pidx.end()) rn = pit->second;
    }
    if (rn == nullptr || rn->rfind("_ZTV", 0) != 0) return false;

    uint64_t* slots = reinterpret_cast<uint64_t*>(cell);
    const uint64_t safe = reinterpret_cast<uint64_t>(&NativePureVirtual);
    // Ilk iki slot RTTI alanidir (offset-to-top, typeinfo); onlari 0 birakiyoruz
    // ki tip sorgulari "bilinmiyor" gorsun. Geri kalani sanal fonksiyon slotu.
    const size_t n = bytes / 8;
    for (size_t i = 0; i < n; i++) {
        slots[i] = (i < 2) ? 0ull : safe;
    }
    static std::atomic<int> s_vt{0};
    if (s_vt.fetch_add(1) < 8) {
        printf("[VTABLE] %s -> %zu slot guvenli fonksiyona baglandi\n", rn->c_str(), n);
        fflush(stdout);
    }
    return true;
}

// plt_index -> native fonksiyon (yoksa nullptr => eski sentinel/VEH yolu).
// linker.cpp GOT'u yamarken cagirir.
extern "C" void* PsemuNativePltStub(int plt_index) {
    static const bool s_enabled = [] {
        const char* e = getenv("PSEMU_NATIVE_PLT");
        return !(e != nullptr && e[0] == '0');
    }();
    if (!s_enabled) return nullptr;

    auto it = g_plt_names.find(plt_index);
    if (it == g_plt_names.end()) return nullptr;

    // NID -> okunabilir isim (tam eslesme, yoksa 11 karakterlik onek).
    const std::string* rn = nullptr;
    auto exact = g_nid_to_name.find(it->second);
    if (exact != g_nid_to_name.end()) {
        rn = &exact->second;
    } else if (it->second.size() >= 11) {
        const auto& pidx = NidPrefixIndex();
        auto pit = pidx.find(it->second.substr(0, 11));
        if (pit != pidx.end()) rn = pit->second;
    }
    if (rn == nullptr) return nullptr;

    const std::string& n = *rn;
    if (n == "memcpy")  return reinterpret_cast<void*>(&NativeMemcpy);
    if (n == "memset")  return reinterpret_cast<void*>(&NativeMemset);
    if (n == "memmove") return reinterpret_cast<void*>(&NativeMemmove);
    if (n == "memcmp" || n == "bcmp") return reinterpret_cast<void*>(&NativeMemcmp);
    if (n == "scePthreadMutexLock" || n == "pthread_mutex_lock")
        return reinterpret_cast<void*>(&NativeMutexLock);
    if (n == "scePthreadMutexUnlock" || n == "pthread_mutex_unlock")
        return reinterpret_cast<void*>(&NativeMutexUnlock);
    // sceKernelWaitSema ARTIK NATIVE DEGIL: gercek semafor implementasyonu
    // geldi (bloklamasi ve zaman asimini onurlandirmasi gerekiyor), o yuzden
    // VEH'teki erken dala gitmeli.
    if (n == "__error")           return reinterpret_cast<void*>(&NativeErrno);
    if (n == "strlen") return reinterpret_cast<void*>(&NativeStrlen);
    // DENENDI VE GERI ALINDI (2026-07-27): scePthreadGetthreadid.
    // Gerekce makuldu - Astro Bot'un is parcaciklari onu siki bir dongude
    // cagiriyor ve her cagri bir exception turu. Native'e alindi, GOT'a
    // gercekten yazildi (native slot 11 -> 12, PLT#72 artik hic [PLT-HLE]
    // uretmiyor). AMA CPU yuku HIC DEGISMEDI (25 sn'de ~230 sn, ~9 cekirdek):
    // yani cekirdekleri yakan sey bu degilmis. Ustelik ayni derlemede
    // Dreaming Sarah T+2.31'de takildi (oncesinde 12.5 FPS oynuyordu).
    // Faydasi olculemeyen, riski olcumle gorunen bir degisikligi tutmuyoruz.
    // Yukaridaki notun uyardigi tam da bu: bu liste tek tek ve BIRDEN FAZLA
    // kosuyla genisletilmeli.
    // LISTEYI GENISLETME DENEMESI GERI ALINDI (2026-07-26):
    // libc_char_table (3.700 cagri/sn), sceKernelUsleep ve strcmp eklendiginde
    // kosular erken cokmeye basladi. ANCAK bunu onlara guvenle YUKLEYEMIYORUM:
    // temel de aralikli olarak erken takiliyor/cokuyor (ayni oturumda native
    // PLT'den ONCE de 398/402/408 cagride olen kosular oldu). Tek kosuya bakip
    // suclu ilan etmek yanlis olur. Once o kararsizlik cozulmeli, sonra liste
    // tek tek ve her biri icin BIRDEN FAZLA kosuyla genisletilmeli.
    // Yukaridaki set 12 FPS ile dogrulanmis olan settir.
    return nullptr;
}

static GuestCond* GetOrCreateCond(uint64_t* slot) {
    if (slot == nullptr || !SafeWritable(slot, sizeof(uint64_t))) return nullptr;
    std::lock_guard<std::mutex> lk(g_sync_create_mutex);
    uint64_t h = *slot;
    if (h != 0 && g_known_conds.count(h) != 0) {
        return reinterpret_cast<GuestCond*>(h);
    }
    GuestCond* c = new GuestCond();
    InitializeConditionVariable(&c->cv);
    *slot = reinterpret_cast<uint64_t>(c);
    g_known_conds.insert(*slot);
    return c;
}

// Global degiskenler
uint64_t g_game_thread_entry = 0;

// 0x2dfff0 (tip-kayit) trampoline'inin cagri sayacina isaretci; loader
// kurar. Crash aninda "kac kez cagrildi"yi loglamak icin.
volatile uint32_t* g_reg_call_count_ptr = nullptr;

// ========================================================
// UTF16 CONVERTER DETOUR TANISI (thread-guvenli)
// ========================================================
// Loader, string-format cagri yerini (0x17b818 -> 0x17b120) bir
// trampoline'e yonlendirir; trampoline once bunu cagirir (SysV: value RCX),
// sonra orijinal converter'i. Amac: bozuk surrogate iceren gercek degerin
// TIP ETIKETINI ([value+8]&0xf) ve u16string basligini gormek -> tip
// karisikligi (sayi string sanilyor) mi yoksa gercekten bozuk string mi.
extern "C" void Utf16DiagValue(void* value) {
    if (value == nullptr) return;
    auto Readable = [](const void* p, size_t n) -> bool {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
        const uint8_t* end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        return reinterpret_cast<const uint8_t*>(p) + n <= end;
    };
    uint8_t* vs = reinterpret_cast<uint8_t*>(value) + 0x10;
    if (!Readable(vs, 0x18)) return;
    // std::u16string layout'u belirsiz (alt/std/SSO). Uc yorumu da dene ve
    // ISARET EDILEN karakterlerde (pointer baytlarinda DEGIL) eslesmemis
    // lead surrogate ara. Hangisi bulursa gercek bozuk string odur.
    struct C { const uint16_t* d; uint64_t n; const char* how; };
    uint64_t p0 = *reinterpret_cast<uint64_t*>(vs);       // +0
    uint64_t p8 = *reinterpret_cast<uint64_t*>(vs + 8);   // +8
    uint64_t p16 = Readable(vs + 16, 8) ? *reinterpret_cast<uint64_t*>(vs + 16) : 0;
    C cands[3] = {
        { reinterpret_cast<const uint16_t*>(p0),  p8,  "alt(ptr@0,len@8)" },   // alternatif layout
        { reinterpret_cast<const uint16_t*>(p16), p8,  "std(ptr@16,len@8)" },  // standart layout
        { reinterpret_cast<const uint16_t*>(vs),  static_cast<uint64_t>((*vs) >> 1), "SSO(inline)" }, // kisa string
    };
    // ============================================================
    // ONEMLI: Bu fonksiyon artik VARSAYILAN OLARAK BELLEGE YAZMAZ.
    // ------------------------------------------------------------
    // Eski hali yukaridaki UC layout tahmininden hangisinde "eslesmemis
    // surrogate" bulursa oraya '?' (0x003F) yaziyordu. Ama tahmin YANLIS
    // oldugunda p0/p16 rastgele bir adres olarak yorumlanip ALAKASIZ
    // bellegi bozuyordu.
    // Kanit zinciri (loader_log.txt):
    //   - 9 kez "len=2 layout=alt(ptr@0,len@8)" tetiklendi,
    //   - dil kodu "us" (tam 2 karakter) bozuldu:
    //     "ON SAVEGAME MISSING (langcode is us)" -> "(langcode is )",
    //   - ardindan lokalizasyon anahtarlari bulunamadi
    //     ("KEY NOT FOUND: music_atten / master_atten"),
    //   - sonuc: ana menu metinleri bos.
    // Ayrica loader invalid_utf16 throw dallarini zaten NOP'luyor
    // ([UTF16-NONFATAL]), yani bozuk string CRASH uretmiyor -> bu yazma
    // gereksiz. Burasi artik sadece TANI amacli.
    // Eski (yazan) davranis gerekirse: PSEMU_UTF16_FIX=1
    // ============================================================
    static const bool s_write_fix = (std::getenv("PSEMU_UTF16_FIX") != nullptr);

    for (const C& c : cands) {
        if (c.d == nullptr || c.n == 0 || c.n > 65536) continue;
        if (!Readable(c.d, c.n * 2)) continue;

        bool found = false;
        for (size_t i = 0; i < c.n; i++) {
            const uint16_t ch    = c.d[i];
            const bool     lead  = (ch >= 0xD800 && ch <= 0xDBFF);
            const bool     trail = (ch >= 0xDC00 && ch <= 0xDFFF);
            bool           unpaired = false;
            if (lead) {
                unpaired = (i + 1 >= c.n) ||
                           !(c.d[i + 1] >= 0xDC00 && c.d[i + 1] <= 0xDFFF);
            } else if (trail) {
                unpaired = (i == 0) ||
                           !(c.d[i - 1] >= 0xD800 && c.d[i - 1] <= 0xDBFF);
            }
            if (unpaired) {
                found = true;
                if (s_write_fix) {
                    const_cast<uint16_t*>(c.d)[i] = 0x003F; // '?'
                }
            }
        }

        if (found) {
            static volatile LONG s_n = 0;
            if (InterlockedIncrement(&s_n) <= 16) { // rate-limit (bkz. DEVELOPER_GUIDE)
                std::cout << "[UTF16] eslesmemis surrogate goruldu: len=" << c.n
                          << " layout=" << c.how
                          << (s_write_fix ? " (YAZILDI - PSEMU_UTF16_FIX)" : " (sadece tani, yazilmadi)")
                          << std::endl;
            }
            return;
        }
    }
}

// NOT: free karantinasi (geciktirilmis free) DENENDI ve ELENDI. Saf sizinti
// (hic free etme) ile bile item-aciklamasi u16string bozulmasi surdu -> bu
// UAF bizim allocator'imizin yeniden kullanimindan GELMIYOR (deterministik
// ya da oyunun kendi ic havuzunda). free/realloc normal davranisa dondu.
uint64_t g_base_addr = 0;
uint64_t g_text_size = 0;
uint64_t g_module_size = 0;
uint64_t g_plt8_param_ptr = 0;
uint64_t g_plt8_param_size = 0;
uint64_t g_original_entry = 0;
uint64_t g_real_process_param = 0;
std::map<int, std::string> g_plt_names;
uint64_t g_procparam_vaddr = 0;

// DT_INIT: module_start'tan ONCE cagirilmasi gereken CRT baslatici
// (.init_array/statik constructor yurutucusu). Daha once bu hic
// cagirilmiyordu - RVA 0x2c61b2 cokmesindeki gibi initialize edilmemis
// globallerin gercek kaynagi buydu.
uint64_t g_init_vaddr = 0;

// PT_TLS sablonu (loader.cpp'den gelir)
static uint64_t g_tls_vaddr = 0;
static uint64_t g_tls_filesz = 0;
static uint64_t g_tls_memsz = 0;
static uint64_t g_tls_align = 0;

// Gercek TLS blogunun thread pointer (tp) adresi. *(tp) = tp (Variant II self-pointer).
uint64_t g_tls_base = 0;

// TLS sablonu (thread basina blok uretmek icin saklanir)
static uint64_t g_tls_template_src = 0; // base_addr + tls_vaddr
static uint64_t g_tls_align_v      = 0;

// ============================================================
// THREAD BASINA TLS
// ============================================================
// Oyun 40'tan fazla thread aciyor ve libc'nin allocator'i boyut-sinifi
// basina serbest-liste basini TLS'te tutuyor (RVA 0x104f61:
// mov r14, [fs:0 + rbx*8 - 0x1870]). Bu listeler thread-local
// varsayildigi icin KILITSIZ kullaniliyor. Tek bir global blogu
// paylastirirsak thread'ler birbirinin listesini bozuyor ve bozuk
// pointer (-1) okunup RVA 0x104f7d'de cokme oluyordu.
static uint64_t CreateTlsBlockForCurrentThread() {
    if (g_tls_memsz == 0) return 0;
    uint64_t align        = g_tls_align_v ? g_tls_align_v : 8;
    uint64_t aligned_size = (g_tls_memsz + (align - 1)) & ~(align - 1);
    constexpr uint64_t TCB_SIZE  = 0x40;
    constexpr uint64_t TCB_ALIGN = 0x20;
    // libc thread-local allocator negative offsets reach up to -0x1870 or more.
    // Ensure tcb_offset is at least 0x10000 bytes so negative offsets stay inside blk.
    uint64_t tcb_offset = std::max<uint64_t>(aligned_size, 0x10000);
    uint64_t total_size = ((tcb_offset + (TCB_ALIGN - 1)) & ~(TCB_ALIGN - 1)) + TCB_SIZE;

    uint8_t* blk = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!blk) return 0;

    memset(blk, 0, total_size);
    uint8_t* tls_data_start = blk + (tcb_offset - aligned_size);
    if (g_tls_filesz > 0 && g_tls_template_src != 0) {
        memcpy(tls_data_start, reinterpret_cast<void*>(g_tls_template_src), g_tls_filesz);
    }
    uint64_t tp = reinterpret_cast<uint64_t>(blk) + tcb_offset;
    *reinterpret_cast<uint64_t*>(tp) = tp; // Variant II: *(tp) = tp
    return tp;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("fsgsbase")))
static inline void SafeWriteFsBase(uint64_t val) {
    _writefsbase_u64(val);
}
#else
static inline void SafeWriteFsBase(uint64_t val) {
    _writefsbase_u64(val);
}
#endif

// ========================================================
// TLS HIZLANDIRMA: fs:[0] -> gs:[TEB TlsSlots+N]
// --------------------------------------------------------
// PS5 ELF'i thread pointer'i Linux tarzi "mov reg, fs:[0]" ile okur. Windows'ta
// FS tabani kullanici modunda kalici DEGILDIR: wrfsbase ile yazilan deger ilk
// baglam degisiminde sifirlanir (bu makinede olculdu: Sleep(20) sonrasi
// rdfsbase=0, 200/200 tur basarisiz). Dolayisiyla her fs: erisimi bir access
// violation -> VEH gidis-donusu demek; JS motoru gibi TLS'i yogun kullanan kod
// bunun altinda eziliyor.
//
// Cozum: komutu YERINDE yamalamak. Windows'ta GS tabani TEB'i gosterir ve TEB'in
// TlsSlots dizisi (ofset 0x1480) thread'e ozeldir - yani TAM olarak ihtiyacimiz
// olan sey. Iki kodlama ayni uzunluktadir:
//     64 48 8B 04 25 00 00 00 00   mov rax, fs:[0]
//     65 48 8B 04 25 80 14 00 00   mov rax, gs:[0x1480]
// Sadece segment on eki (0x64->0x65) ve disp32 degisir. Yamadan sonra o komut
// bir daha ASLA exception uretmez ve her thread kendi tp'sini okur.
// ========================================================
static const uint32_t kTebTlsSlotsOffset = 0x1480; // TEB.TlsSlots[0]
static DWORD    g_win_tls_slot = TLS_OUT_OF_INDEXES;
static uint32_t g_teb_slot_off = 0; // 0 => yama devre disi, VEH yolu kullanilir

// Oyun baslamadan once (VEH kaydiyla ayni yerde) cagrilir: VEH icinde
// TlsAlloc gibi kilit alan is yapmak istemiyoruz.
void PsemuInitTlsFastPath() {
    if (g_teb_slot_off != 0) return;
    // VARSAYILAN: KAPALI.
    // Bu yama TLS fault'larini binlerce/sn'den ~0'a indiriyor AMA olculdu ki
    // oyunu kararsizlastiriyor: 5 kosunun 5'i T+11-22 arasinda cokuyor
    // (READ @ 0x0 / kanonik olmayan adres), kapaliyken 5/5 sorunsuz boot
    // ediyor. Bos TEB slotu hipotezi test edildi ve ELENDI (slot garantisi +
    // her thread icin TLS geri cagrisi eklendi, yine 5/5 coktu). Kalan en
    // guclu aciklama: calisan kodu diger cekirdekler o komutu isletirken
    // degistirmek (cross-modifying code) x86'da senkronizasyon gerektirir.
    // Dogru cozum muhtemelen yamayi ONCEDEN, misafir thread'leri baslamadan
    // statik taramayla uygulamak. O yapilana kadar varsayilan kapali.
    // Acmak icin: PSEMU_TLS_PATCH=1
    const char* e = getenv("PSEMU_TLS_PATCH");
    if (e == nullptr || e[0] == '0') {
        printf("[TLS] Hizli yol KAPALI (varsayilan; PSEMU_TLS_PATCH=1 ile acilir) - "
               "her fs:[0] VEH'e dusecek\n");
        fflush(stdout);
        return;
    }
    DWORD s = TlsAlloc();
    if (s == TLS_OUT_OF_INDEXES) return;
    if (s < 64) { // >= 64 ise TlsExpansionSlots'a taser; sabit TEB ofseti olmaz
        // KENDI KENDINI TEST: TEB.TlsSlots ofsetinin (0x1480) bu Windows
        // surumunde gercekten dogru oldugunu VARSAYMAK yerine dogruluyoruz.
        // Yanlis olsaydi yamali komutlar TEB'in baska bir alanini okur ve
        // tesadufi cop degerler uretirdi - teshis edilmesi cok zor bir hata.
        const uint32_t off = kTebTlsSlotsOffset + 8u * s;
        const uint64_t magic = 0x5053454D55544C53ull; // "PSEMUTLS"
        TlsSetValue(s, reinterpret_cast<void*>(magic));
        uint64_t read_back = 0;
        __asm__ volatile("movq %%gs:(%1), %0" : "=r"(read_back) : "r"(static_cast<uint64_t>(off)));
        TlsSetValue(s, nullptr);
        if (read_back != magic) {
            TlsFree(s);
            printf("[TLS] Hizli yol KAPALI: gs:[0x%x] TlsSetValue ile uyusmuyor "
                   "(okunan 0x%llx, beklenen 0x%llx) - TEB duzeni farkli\n",
                   off, static_cast<unsigned long long>(read_back),
                   static_cast<unsigned long long>(magic));
            fflush(stdout);
            return;
        }
        g_win_tls_slot = s;
        g_teb_slot_off = off;
        printf("[TLS] Hizli yol etkin: fs:[0] -> gs:[0x%x] (TLS slot %lu, dogrulandi)\n",
               g_teb_slot_off, s);
    } else {
        TlsFree(s);
        printf("[TLS] Hizli yol KAPALI: TlsAlloc slot %lu >= 64\n", s);
    }
    fflush(stdout);
}

// Bu thread'in tp'si; ilk fs: erisiminde olusturulur.
static uint64_t GetThreadTlsBase() {
    static thread_local uint64_t t_tp = 0;
    if (t_tp == 0) {
        t_tp = CreateTlsBlockForCurrentThread();
        if (t_tp != 0) {
            if (g_tls_base == 0) g_tls_base = t_tp; // ilk blok: geriye uyumluluk
            // Yamalanmis komutlarin okudugu yer: bu thread'in TEB slotu.
            // Yama fault uretmedigi icin slot, thread guest koda girmeden
            // DOLU olmak zorunda (bkz. GamePthreadProc / GuestSortThread).
            static volatile LONG s_n = 0;
            LONG n = InterlockedIncrement(&s_n);
            if (n <= 8) {
                printf("[TLS] Thread'e ozel TLS blogu #%ld: TID=%lu tp=0x%llx\n",
                       n, GetCurrentThreadId(), t_tp);
                fflush(stdout);
            }
        }
    }
    // Slot HER cagride garantiye alinir. Yamali komut dogrudan TEB slotunu
    // okur; slot bossa thread pointer 0 cikar ve o thread'in TUM TLS erisimleri
    // cop adrese gider. (Olculdu: yama acikken 5 kosunun 5'i T+13-20 arasinda
    // coktu; kapaliyken 5/5 boot etti.) Ayrica hizli yol, blok yaratildiktan
    // SONRA etkinlesmis olabilir - o durumda ilk kurulumda slot yazilmamis olur.
    if (t_tp != 0 && g_teb_slot_off != 0 && TlsGetValue(g_win_tls_slot) == nullptr) {
        TlsSetValue(g_win_tls_slot, reinterpret_cast<void*>(t_tp));
    }
    return t_tp;
}

// HICBIR THREAD ATLANMASIN: Windows bu geri cagriyi surecte olusan HER thread
// icin calistirir. Boylece guest koda girecek bir thread'in TEB slotu, o thread
// daha ilk komutunu isletmeden dolmus olur. Elle ekledigimiz giris noktalari
// (ExecutionThread/GamePthreadProc/GuestSortThread) yeterli DEGILDI: Kyty'nin
// kendi thread'leri de guest geri cagrilarini isletebiliyor.
static void NTAPI PsemuThreadAttachTls(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_THREAD_ATTACH || reason == DLL_PROCESS_ATTACH) {
        if (g_teb_slot_off != 0) {
            GetThreadTlsBase();
        }
    }
}

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK psemu_tls_cb = PsemuThreadAttachTls;

// (fs: komut yamasi TryPatchFsMov, DecodeFsMov'un hemen ardinda tanimli.)

// ========================================================
// TANI: Bellek Yazma Izleme Noktasi (Watchpoint)
// ========================================================
// RVA 0x2c61b2'deki "mov rax, [rbx]" cokmesi, dosya-vaddr 0x4942C8'deki
// (DUZELTME: ilk hesaplamada elle yapilan bir hex toplama hatasi yuzunden
// bu adres yanlislikla 0x4A42C8 olarak kullanilmisti - gercek RIP-relative
// hedef 0x2c61ab + 0x1ce11d = 0x4942C8'dir) bir GLOB_DAT relocation
// hedefinin (harici __stack_chk_guard veri sembolu icin GOT slotu)
// loader tarafindan hic yamalanmamasindan kaynaklaniyordu.
static const uint64_t kWatchTargetFileVaddr = 0x4942C8;
static uint64_t g_watch_target = 0;      // calisma zamani adresi (g_base_addr + kWatchTargetFileVaddr)
static void*    g_watch_page = nullptr;  // PAGE_GUARD uygulanan sayfa basi
static size_t   g_watch_page_size = 0;
static DWORD    g_watch_orig_protect = 0;
static int      g_watch_hits = 0;
static const int kWatchHitLimit = 30;    // log/re-arm siniri (spam onleme)
static bool     g_watch_rearm_pending = false;

// TANI breakpoint: utf16 donusum girisine (0x17b120) INT3 koyup, kaynak
// string'de eslesmemis surrogate olan cagriyi yakalayip dokecegiz.
static uint64_t g_diag_bp_addr    = 0;
static uint8_t  g_diag_bp_orig    = 0;
static bool     g_diag_bp_pending = false; // single-step sonrasi 0xCC'yi geri koy

// ---------------------------------------------------------------------------
// .init_array IZLEYICISI  (PSEMU_INIT_TRACE=<call rax RVA'si>, or. 0x62)
//
// Oyunun CRT'sindeki .init_array yuruyucusu su sekilde:
//     0x50: add rbx, -8
//     0x54: mov rax, [rbx]      <- rbx = imlec, rax = ilklendirici
//     0x62: call rax            <- FF D0
//     0x64: jmp 0x50
// "call rax" uzerine 0xCC koyup VEH'de yakaliyoruz: hangi girdinin
// calistigini (indeks + RVA) biliyoruz, sonra cagriyi ELDE canlandiriyoruz
// (donus adresini it, RIP=RAX). Boylece orijinal bayti geri koymaya ve
// single-step ile yeniden kurmaya gerek kalmiyor - thread-guvenli.
// ---------------------------------------------------------------------------
static uint64_t g_initcall_addr  = 0;   // "call rax" komutunun mutlak adresi
static uint64_t g_initcall_next  = 0;   // komutun hemen sonrasi (donus adresi)
static uint64_t g_initcall_n     = 0;   // kacinci ilklendirici
static uint64_t g_initcall_last  = 0;   // en son cagrilan ilklendiricinin RVA'si
static uint64_t g_initcall_cursor = 0;  // yuruyucunun imleci (RVA)
static bool     g_initcall_verbose = false; // PSEMU_INIT_TRACE_LOG=1
static uint64_t g_initcall_r15   = 0;   // callee-saved izleme
static uint64_t g_initcall_r14   = 0;
static uint64_t g_initcall_prev  = 0;   // bir onceki ilklendiricinin RVA'si
uint64_t        g_expected_argv  = 0;   // module_start'a gecirilen argv (args_block+8)

// ---------------------------------------------------------------------------
// PROLOGUE BREAKPOINT'i  (PSEMU_BP=<rva>, virgulle en fazla 4 tane)
//
// Hedef RVA "push rbp" (0x55) ile basliyorsa oraya 0xCC koyup VEH'de
// yakaliyoruz: tum registerlari ve [RSP]'deki DONUS ADRESINI (yani cagirani)
// dokuyoruz, sonra "push rbp"yi elde canlandirip devam ediyoruz. Orijinal
// bayti geri koymak / single-step ile yeniden kurmak gerekmiyor.
// ---------------------------------------------------------------------------
static uint64_t g_bp_addr[4] = {0, 0, 0, 0};
static int      g_bp_count = 0;
static int      g_bp_hits[4] = {0, 0, 0, 0};
static const int kBpLogLimit = 8;   // her breakpoint icin en fazla bu kadar log

// ---------------------------------------------------------------------------
// TEK ADIM IZLEME  (PSEMU_TRACE_FROM=<rva>)
//
// Verilen fonksiyona girildiginde TF (trap flag) kurulur; ondan sonraki her
// komutun RIP'i halka tamponuna yazilir (LOG YOK - her adimda log yazmak
// isi tamamen durdurur). Cokme aninda son kTraceRing adres dokulur; boylece
// "RIP nereden bu cop adrese atladi" sorusu KESIN olarak cevaplanir.
// Adim siniri asilirsa izleme kendini kapatir (oyun donmasin).
// ---------------------------------------------------------------------------
static const int kTraceRing = 64;
static uint64_t  g_trace_from   = 0;     // izlemeyi baslatan fonksiyonun adresi
static bool      g_trace_active = false;
static uint64_t  g_trace_ring[kTraceRing] = {0};
static uint64_t  g_trace_pos    = 0;
static uint64_t  g_trace_steps  = 0;
static uint64_t  g_trace_max    = 3000000; // PSEMU_TRACE_MAX ile degistirilebilir

// ---------------------------------------------------------------------------
// YIGIN KANARYASI  (PSEMU_CANARY=<rva>, o RVA ayrica PSEMU_BP'de olmali)
//
// Verilen fonksiyona girildiginde "push r15"in yazacagi yigin gozunun
// ADRESI ve degeri saklanir. Bundan sonra HER HLE cagrisinin donusunde o
// goz kontrol edilir; degisirse degistiren cagriyi ISMIYLE bildiririz.
// Callee-saved bir registerin kaydedildigi goz, fonksiyon yasadigi surece
// SABIT kalmak zorundadir - degisiyorsa biri yiginin uzerine yaziyor.
// ---------------------------------------------------------------------------
static uint64_t g_canary_bp   = 0;
static uint64_t g_canary_addr = 0;
static uint64_t g_canary_val  = 0;
static int      g_canary_hits = 0;

// Tek adim izleme sirasinda: R15 DOGRU argv degerini kaybettigi/geri
// kazandigi anlari kaydet. "1" gibi yaygin degerleri izlemek yerine
// dogrudan beklenen argv'yi izledigimiz icin gurultusuz.
struct R15Event { uint64_t rip, from, to; };
static const int kR15Ring = 16;
static R15Event  g_r15_ring[kR15Ring] = {};
static uint64_t  g_r15_pos  = 0;
static uint64_t  g_r15_prev = 0;

// ---------------------------------------------------------------------------
// DONANIM YAZMA KESME NOKTASI  (PSEMU_WATCH_WRITE=<rva>)
//
// Bir kod/veri adresine YAZAN komutu bulmak icin DR0 kullaniyoruz. PAGE_GUARD
// bir kod sayfasinda ise yaramaz (her calisma tetikler); donanim izleme
// sadece YAZMADA tetikleniyor. DR7: L0=1, R/W0=01 (yazma), LEN0=11 (4 bayt,
// adres 4'e hizali olmali).
// ---------------------------------------------------------------------------
static uint64_t g_watchw_addr  = 0;
static bool     g_watchw_armed = false;
static int      g_watchw_hits  = 0;

// ---------------------------------------------------------------------------
// KOD SAYFALARINI SALT-OKUNUR YAP  (PSEMU_PROTECT_CODE=1)
//
// Astro Bot'ta 0xe3814'teki "call 0x24ae30" komutunun rel32'si calisirken
// uzerine yaziliyor (olculdu: dosya E8 17 76 16 00, bellek E8 C7 AE 0F 00).
// DR0 yazma izlemesi misafir thread'inde TETIKLENMEDI - demek ki yazan
// baska bir thread. Debug registerlari thread'e ozel oldugu icin onlari
// goremiyor. Kod araligini PAGE_EXECUTE_READ yapinca HANGI thread olursa
// olsun yazma erisim ihlaline dusuyor ve yazani RIP+TID ile yakaliyoruz.
// Yakaladiktan sonra sayfayi yazilabilir yapip devam ediyoruz (oyun
// durmasin); her sayfa icin ilk yazma bildiriliyor.
// ---------------------------------------------------------------------------
static bool     g_protect_code = false;
static int      g_codew_hits   = 0;

// ---------------------------------------------------------------------------
// BOS ISARETCIYE YAZMAYI ATLA  (PSEMU_SKIP_NULL_STORE=1)
//
// Bring-up araci: oyun ayrilmamis bir listeye yazmaya calisiyorsa (NULL +
// kucuk ofset) o TEK komutu atlayip devam ediyoruz. Amac bir sonraki gercek
// engeli gorebilmek; kalici bir duzeltme DEGIL, bu yuzden varsayilan KAPALI
// ve her atlama loglaniyor.
//
// Komut uzunlugunu bulmak icin kucuk bir x86-64 "store" cozucusu: onekler +
// opcode + ModRM + SIB + disp + imm. Yalnizca MOV-store bicimleri
// (88/89/C6/C7) ve yaygin SSE/AVX store'lari icin dogru; tanimadigi bicimde
// 0 donup atlamayi reddediyor (yanlis uzunlukla ilerlemek felaket olurdu).
// ---------------------------------------------------------------------------
static bool g_skip_null_store = false;
static int  g_skip_hits = 0;

static size_t DecodeStoreLen(const uint8_t* p) {
    size_t i = 0;
    bool opsize16 = false;
    // Eski onekler + REX
    for (int k = 0; k < 8; k++) {
        uint8_t b = p[i];
        if (b == 0x66) { opsize16 = true; i++; continue; }
        if (b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 ||
            b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { i++; continue; }
        if (b >= 0x40 && b <= 0x4F) { i++; continue; }
        break;
    }
    uint8_t op = p[i++];
    int imm = 0;
    if (op == 0x88 || op == 0x89) {          // mov r/m, r
        imm = 0;
    } else if (op == 0xC6) {                 // mov r/m8, imm8
        imm = 1;
    } else if (op == 0xC7) {                 // mov r/m32, imm32
        imm = opsize16 ? 2 : 4;
    } else if (op == 0x0F) {                 // 0F 11 = movups/movupd store
        uint8_t op2 = p[i++];
        if (op2 != 0x11 && op2 != 0x29 && op2 != 0x7F) return 0;
    } else {
        return 0;                            // tanimadik: atlama YOK
    }
    uint8_t modrm = p[i++];
    uint8_t mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return 0;                  // register hedefi: store degil
    if (rm == 4) i++;                        // SIB
    if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
    else if (mod == 0 && rm == 5) i += 4;    // rip-relative
    else if (mod == 0 && rm == 4 && (p[i - 1] & 7) == 5) i += 4; // SIB base yok
    return i + imm;
}

static void ArmWatchpoint() {
    if (g_watch_page == nullptr) return;
    DWORD oldProt;
    VirtualProtect(g_watch_page, g_watch_page_size, g_watch_orig_protect | PAGE_GUARD, &oldProt);
}

// StartExecution icinden cagirilir: g_base_addr belli olduktan sonra hedef
// adresi hesaplar, sayfa korumasini sorgular ve PAGE_GUARD ekler.
static void SetupWatchpoint(uint64_t base_addr) {
    g_watch_target = base_addr + kWatchTargetFileVaddr;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t page_size = si.dwPageSize;
    uint64_t page_base = g_watch_target & ~(page_size - 1);

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(page_base), &mbi, sizeof(mbi)) == 0) {
        LOG_ERROR("[WATCHPOINT] VirtualQuery basarisiz, izleme kurulamadi.");
        return;
    }

    g_watch_page = reinterpret_cast<void*>(page_base);
    g_watch_page_size = static_cast<size_t>(page_size);
    g_watch_orig_protect = static_cast<DWORD>(mbi.Protect);
    g_watch_hits = 0;

    ArmWatchpoint();

    std::stringstream ss;
    ss << "[WATCHPOINT] 0x" << std::hex << g_watch_target
       << " (dosya-vaddr 0x" << kWatchTargetFileVaddr << ") icin PAGE_GUARD kuruldu. Sayfa=0x"
       << page_base << " boyut=0x" << page_size << std::dec;
    LOG_INFO(ss.str());
}

// ========================================================
// SceProcessParam / SceLibcParam icin guvenli bellek blogu
// (MSVC derleyici hatalarini onlemek icin global alana alindi)
// ========================================================
#pragma pack(push, 1)
struct SceProcessParamBlock {
    uint32_t magic;
    uint32_t data1;
    uint32_t data2;
};

struct SceProcessParam {
    uint64_t safe_pointers[6];         // 0x00 - 0x2F (Guvenli pointerlar)
    SceProcessParamBlock* block_array; // 0x30
    uint64_t block_count;              // 0x38
};
#pragma pack(pop)

static const uint32_t all_magics[23] = {
    0x6AC156EF, 0x6AC15610, 0x6AC15009, 0x6AC153BA,
    0xBE7DCD73, 0x0C4B1438, 0xDB00D71A, 0xDB00D249,
    0xDB00EC60, 0x8FB4EDB5, 0xB994AD29, 0xD427322F,
    0xF58FEA31, 0x0C4D6FE4, 0x0C4A80EF, 0x0DD283E7,
    0xC620E68C, 0xC67EFACF, 0xD9E6D9F7, 0x31F34B9F,
    0xAC0F9E76, 0x929FD95D, 
    0x19E93E85 // [EKLENDI] Yeni sihirli sayi
};


// Helper: Adresin oyunun .text segmenti icerisinde olup olmadigini kontrol eder
static bool IsInTextSegment(uint64_t addr) {
    return (addr >= g_base_addr && addr < g_base_addr + g_text_size);
}

// Helper: Adresin oyun icin tahsis edilen TUM bellek blogu (module_size) icinde
// olup olmadigini kontrol eder. PLT/Non-PLT RET simulasyonunda stack'in tepesinden
// okunan "donus adresi"nin gercekten oyun moduluna ait olup olmadigini dogrulamak
// icin kullanilir - degilse (ornegin loader.exe'nin kendi native kod bolgesine
// denk geliyorsa) kor bir sekilde oraya sicramak cok daha kotu, izlemesi zor bir
// cokmeye yol acar; bunun yerine acikca "gecersiz RET adresi" olarak raporlanir.
static bool IsInModuleRange(uint64_t addr) {
    return (addr >= g_base_addr && addr < g_base_addr + g_module_size);
}

// e_entry thread'i icin placeholder fonksiyon
static DWORD WINAPI GameEntryThreadFunc(LPVOID) {
    while (true) Sleep(1000);
    return 0;
}

// ========================================================
// System V AMD64 ABI Trampoline (dosya kapsaminda - hem ExecutionThread
// hem de VEH handler icindeki scePthreadCreate tarafindan kullanilir)
// ========================================================
// Calisma zamaninda makine kodu uretip PAGE_EXECUTE_READWRITE bir bloga
// yazar:  mov rdi,arg0 ; mov rsi,arg1 ; mov rdx,arg2 ; mov rax,target ; jmp rax
static void* BuildSysVTramp(uint64_t target, uint64_t rdi_val, uint64_t rsi_val, uint64_t rdx_val) {
    uint8_t* stub = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return nullptr;
    int off = 0;
    stub[off++] = 0x48; stub[off++] = 0xBF; memcpy(&stub[off], &rdi_val, 8); off += 8; // mov rdi
    stub[off++] = 0x48; stub[off++] = 0xBE; memcpy(&stub[off], &rsi_val, 8); off += 8; // mov rsi
    stub[off++] = 0x48; stub[off++] = 0xBA; memcpy(&stub[off], &rdx_val, 8); off += 8; // mov rdx
    stub[off++] = 0x48; stub[off++] = 0xB8; memcpy(&stub[off], &target, 8);  off += 8; // mov rax
    stub[off++] = 0xFF; stub[off++] = 0xE0;                                             // jmp rax
    return stub;
}

// scePthreadCreate ile olusturulan gercek Windows thread'inin giris noktasi.
// lpParam = SysV trampoline stub'i (icinde entry+arg gomulu). Stub'i cagirir;
// SysV fonksiyonu RSI/RDI'yi bozsa bile Win64 caller (bu fonksiyon) kendi
// stack slotlarindan geri yukledigi icin sorun olmaz.
static DWORD WINAPI GamePthreadProc(LPVOID lpParam) {
    ULONG guarantee = 512 * 1024; // 512KB
    SetThreadStackGuarantee(&guarantee);

    uint64_t tp = GetThreadTlsBase();
    if (tp != 0) {
        __try {
            SafeWriteFsBase(tp);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    typedef int(*Fn)();
    Fn fn = reinterpret_cast<Fn>(lpParam);
    if (fn) fn();
    return 0;
}

// ========================================================
// HANG WATCHDOG: Worker thread belli sure aktivite gostermezse
// (PLT cagrisi/fault yok) muhtemel bir spin-loop/deadlock'tur.
// Thread'i askiya alip RIP + register + komut baytlarini dokerek
// tam olarak NEREDE takildigini kanitlar (tahmin degil).
// ========================================================
static volatile ULONGLONG g_last_activity = 0;   // GetTickCount64, her PLT cagrisinda tazelenir
static HANDLE   g_worker_thread = nullptr;         // scePthreadCreate ile olusturulan worker
static int      g_watchdog_dumps = 0;
static const int kWatchdogMaxDumps = 15;           // en fazla bu kadar dok (spam onleme)

// Ana misafir thread'i (ExecutionThread). Worker yokken watchdog bunu izler.
static HANDLE g_exec_thread = nullptr;

// ---------------------------------------------------------------------------
// TUM THREAD'LERI ORNEKLE  (PSEMU_THREAD_SAMPLE=<saniye>)
//
// Mevcut watchdog yalnizca TEK bir thread'e bakiyor. Astro Bot artik 19
// misafir thread'i aciyor ve log filtresi tekrarlari susturdugu icin
// "sessizlik" neyin bekledigini soylemiyor. Bu ornekleyici periyodik olarak
// SURECTEKI TUM thread'lerin RIP'ini RVA olarak doker; boylece hangi
// thread'in nerede dondugunu/bekledigini goruyoruz.
//
// Kilitlenmeyi onlemek icin once hepsi ornekleniyor ve HEMEN devam
// ettiriliyor; loglama ancak butun thread'ler serbest birakildiktan sonra
// yapiliyor (askidayken log kilidini beklemek olumcul olurdu).
// ---------------------------------------------------------------------------
// Her thread'in EN SON girdigi HLE fonksiyonu. Ornekleyici baska bir
// thread'in thread_local'ini okuyamaz, o yuzden kucuk bir global tablo.
// Kilit yok: yazan tek thread kendi slotudur, okuyan sadece raporlar.
struct ThreadHle { std::atomic<DWORD> tid{0}; std::atomic<int> plt{-1}; };
static ThreadHle g_thread_hle[64];

// SICAK YOL: saniyede yuz binlerce kez calisiyor, bu yuzden sadece TEK bir
// tam sayi yaziyor. Slot bir kez alinip thread_local'da saklaniyor (her
// cagrida 64'luk tabloyu taramak oyunu belirgin sekilde yavaslatiyordu).
// Isim burada tutulmuyor; ornekleyici PLT indeksini basar, indeks->isim
// cevirisi tools/scripts/plt_entry.py ile yapilir.
static thread_local int t_hle_slot = -1;

static inline void RecordThreadHle(int plt_index) {
    if (t_hle_slot < 0) {
        const DWORD tid = GetCurrentThreadId();
        for (int i = 0; i < 64; i++) {
            DWORD expected = 0;
            if (g_thread_hle[i].tid.compare_exchange_strong(expected, tid,
                                                            std::memory_order_relaxed)) {
                t_hle_slot = i;
                break;
            }
        }
        if (t_hle_slot < 0) return; // tablo dolu
    }
    g_thread_hle[t_hle_slot].plt.store(plt_index, std::memory_order_relaxed);
}

static const ThreadHle* FindThreadHle(DWORD tid) {
    for (int i = 0; i < 64; i++)
        if (g_thread_hle[i].tid.load(std::memory_order_relaxed) == tid)
            return &g_thread_hle[i];
    return nullptr;
}

static DWORD WINAPI ThreadSamplerProc(LPVOID param) {
    const DWORD period_ms = static_cast<DWORD>(reinterpret_cast<uintptr_t>(param));
    const DWORD self = GetCurrentThreadId();
    const DWORD pid  = GetCurrentProcessId();
    for (int round = 1;; round++) {
        Sleep(period_ms);
        // DIKKAT: bir thread ASKIDAYKEN BELLEK AYIRMA. Askiya alinan thread
        // CRT yigin kilidini tutuyorsa ayirma sonsuza kadar bloke olur, o
        // thread de hicbir zaman devam ettirilemez -> tum surec kilitlenir.
        // (Bu tam olarak yasandi: ornekleyici acikken kosular T+36'da
        // donuyordu ve "oyun takildi" gibi gorunuyordu.) Bu yuzden sabit
        // boyutlu dizi kullaniyoruz, std::vector degil.
        struct Sample { DWORD tid; uint64_t rip; };
        Sample samples[128];
        int    nsample = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                if (nsample >= 128) break;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                      FALSE, te.th32ThreadID);
                if (h == nullptr) continue;
                if (SuspendThread(h) != (DWORD)-1) {
                    CONTEXT c; memset(&c, 0, sizeof(c));
                    c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &c)) { samples[nsample].tid = te.th32ThreadID; samples[nsample].rip = c.Rip; nsample++; }
                    ResumeThread(h); // ASKIDA LOG YOK: hemen serbest birak
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);

        std::stringstream ss;
        ss << "[THREAD-ORNEK #" << std::dec << round << "] " << nsample
           << " thread:";
        for (int si = 0; si < nsample; si++) { const Sample& s = samples[si];
            ss << "\n    TID=" << std::dec << s.tid << "  RIP=0x" << std::hex << s.rip;
            if (s.rip >= g_base_addr && s.rip < g_base_addr + g_module_size)
                ss << "  (misafir RVA 0x" << (s.rip - g_base_addr) << ")";
            else
                ss << "  (misafir disi)";
            if (const ThreadHle* th = FindThreadHle(s.tid))
                ss << "  son HLE: PLT#" << std::dec << th->plt.load(std::memory_order_relaxed);
        }
        LOG_ERROR(ss.str());

        // Hangi PLT cagrilari CPU yiyor? PsemuDumpPltTop zaten var ama
        // yalnizca PERF yolundan cagriliyordu ve Astro Bot oraya hic
        // varmiyor. Ornekleyici ayri bir thread oldugu icin sicak yolu
        // bozmadan buradan tetikliyoruz.
        PsemuDumpPltTop();
    }
}

static DWORD WINAPI HangWatchdogProc(LPVOID) {
    // Cok agresif: worker sadece ~300ms sessiz kalirsa bile ornek al.
    // Sessiz olum (stack overflow) ihtimaline karsi hizli sampling gerekiyor.
    for (;;) {
        Sleep(150);
        // Worker YOKSA ana misafir thread'ini izle. Eskiden yalnizca
        // scePthreadCreate ile acilmis worker'a bakiyordu; oyun daha worker
        // yaratmadan takilirsa (Astro Bot: e_entry'den hemen sonra tek thread
        // %100 CPU ile donuyor) watchdog hic ornek almiyordu.
        HANDLE target = (g_worker_thread != nullptr) ? g_worker_thread : g_exec_thread;
        if (target == nullptr || g_last_activity == 0) continue;
        if (g_watchdog_dumps >= kWatchdogMaxDumps) continue;

        ULONGLONG now = GetTickCount64();
        if (now - g_last_activity < 300) continue; // hala aktif

        if (SuspendThread(target) == (DWORD)-1) continue;

        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(target, &c)) {
            g_watchdog_dumps++;
            std::stringstream ss;
            ss << "[WATCHDOG #" << g_watchdog_dumps << "] Worker sessiz (>300ms). RIP=0x" << std::hex << c.Rip;
            if (c.Rip >= g_base_addr && c.Rip < g_base_addr + g_module_size) {
                ss << " (RVA 0x" << (c.Rip - g_base_addr) << ")";
                const uint8_t* rb = reinterpret_cast<const uint8_t*>(c.Rip);
                if (SafeReadable(rb, 16)) {
                    ss << " | baytlar: ";
                    for (int i = 0; i < 16; i++) {
                        char b[4]; snprintf(b, sizeof(b), "%02X ", rb[i]); ss << b;
                    }
                }
            }
            ss << " | RSP=0x" << c.Rsp << " RBP=0x" << c.Rbp
               << " RAX=0x" << c.Rax << " RBX=0x" << c.Rbx << " RCX=0x" << c.Rcx
               << " RDX=0x" << c.Rdx << " RSI=0x" << c.Rsi << " RDI=0x" << c.Rdi << std::dec;
            LOG_ERROR(ss.str());

            // Mini stack backtrace: RSP'den yukari tarayip oyun modulune ait
            // donus adreslerini (RVA ile) topla. Ayni RVA'nin tekrar tekrar
            // gorunmesi = OZYINELEME (stack overflow) kaniti.
            std::stringstream bt;
            bt << "  [stack donus adresleri]";
            uint64_t* sp = reinterpret_cast<uint64_t*>(c.Rsp);
            int found = 0;
            for (int i = 0; i < 128 && found < 12; i++) {
                if (!SafeReadable(sp + i, sizeof(uint64_t))) break;
                uint64_t v = sp[i];
                if (v >= g_base_addr && v < g_base_addr + g_module_size) {
                    bt << " 0x" << std::hex << (v - g_base_addr) << std::dec;
                    found++;
                }
            }
            LOG_ERROR(bt.str());
        }
        ResumeThread(target);
    }
    return 0;
}

// ========================================================
// FS: Segment Override Komut Cozucu (Generic TLS Erisim Yakalayici)
// ========================================================
// "mov reg64, fs:[disp32]" bicimindeki komutlari (0..3 adet 66 operand-size
// on eki + zorunlu 64 FS override + opsiyonel REX + 8B opcode + SIB/disp32
// ModRM) calisma zamaninda cozer. ELF TLS erisiminde derleyicinin urettigi
// standart bicim budur (ornegin: 66 66 66 64 48 8B 04 25 00 00 00 00).
struct FsMovInfo {
    int instr_len;
    int dest_reg;   // x86-64 register kodlamasi: 0=RAX ... 7=RDI, 8=R8 ... 15=R15
    int32_t disp;
};

static bool DecodeFsMov(const uint8_t* code, FsMovInfo& info) {
    int i = 0;
    uint8_t rex = 0;

    // Bazi derleyiciler hizalama/redundant amaÃ§larla birden fazla 66 on eki uretebilir
    while (code[i] == 0x66) i++;

    if (code[i] != 0x64) return false; // FS segment override zorunlu
    i++;

    // Opsiyonel REX on eki (0x40-0x4F)
    if ((code[i] & 0xF0) == 0x40) {
        rex = code[i];
        i++;
    }

    if (code[i] != 0x8B) return false; // Sadece MOV r64/r32, r/m destekleniyor
    i++;

    uint8_t modrm = code[i];
    i++;
    uint8_t mod = (modrm >> 6) & 0x3;
    uint8_t reg = (modrm >> 3) & 0x7;
    uint8_t rm  = modrm & 0x7;
    if (rex & 0x4) reg += 8; // REX.R uzantisi

    int32_t disp = 0;
    if (mod == 0 && rm == 4) {
        // SIB byte + saf disp32 (base=101, index=100 -> base/index yok)
        uint8_t sib = code[i];
        i++;
        if ((sib & 0x07) != 0x05 || ((sib >> 3) & 0x07) != 0x04) return false;
        disp = *reinterpret_cast<const int32_t*>(&code[i]);
        i += 4;
    } else if (mod == 1 && rm == 4) {
        // SIB byte + disp8 (base register var)
        i++; // SIB
        disp = static_cast<int8_t>(code[i]);
        i += 1;
    } else if (mod == 2 && rm == 4) {
        // SIB byte + disp32 (base register var)
        i++; // SIB
        disp = *reinterpret_cast<const int32_t*>(&code[i]);
        i += 4;
    } else {
        return false; // Diger adresleme modlari (rip-relative vs.) desteklenmiyor
    }

    info.instr_len = i;
    info.dest_reg = reg;
    info.disp = disp;
    return true;
}

// "64 [REX] 8B 04 25 00000000" komutunu yerinde "65 [REX] 8B 04 25 <teb_off>"
// haline getirir. Sadece disp==0 formunda gecerlidir: yamali hal TEB slotunu
// OKUR (= tp), VEH yolu ise tp+disp hesaplar; bu ikisi yalnizca disp==0 iken
// ayni sonucu verir. (Loglarda olculdu: oyunun TUM fs: erisimleri disp=0.)
//
// Yazma sirasi onemli: once disp32, sonra segment baytini yaziyoruz. Ara
// durumda komut hala fs: oldugu icin fault etmeye devam eder ve VEH tarafi
// disp==g_teb_slot_off ozel durumunu dogru degerle karsilar (bkz. VEH icindeki
// not). Ters sirada yazsaydik "gs:[0]" ara durumu olusurdu; o da TEB'in ilk
// alanini sessizce okur - yanlis deger, fault yok, tespit edilemez.
// Yama neden atlandi? Tahmin etmemek icin ilk birkac vakayi baytlariyla dok.
static void LogPatchSkip(const uint8_t* code, const char* why) {
    static volatile LONG s_n = 0;
    LONG n = InterlockedIncrement(&s_n);
    if (n > 12) return;
    printf("[TLS] Yama ATLANDI (%s) @ 0x%llx baytlar: %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           why, reinterpret_cast<uint64_t>(code), code[0], code[1], code[2], code[3], code[4],
           code[5], code[6], code[7], code[8]);
    fflush(stdout);
}

static void TryPatchFsMov(uint8_t* code, const FsMovInfo& info) {
    if (g_teb_slot_off == 0) return;
    if (info.disp != 0) { LogPatchSkip(code, "disp!=0"); return; }

    int p = 0;
    while (code[p] == 0x66) p++;
    if (code[p] != 0x64) { LogPatchSkip(code, "0x64 yok"); return; }
    const int seg_pos = p;
    p++;
    if ((code[p] & 0xF0) == 0x40) p++;      // opsiyonel REX
    if (code[p] != 0x8B) { LogPatchSkip(code, "opcode 8B degil"); return; }
    p++;
    if ((code[p] & 0xC7) != 0x04) { LogPatchSkip(code, "modrm mod/rm uymuyor"); return; }
    p++;
    if (code[p] != 0x25) { LogPatchSkip(code, "SIB 0x25 degil"); return; }
    p++;
    uint8_t* disp_ptr = &code[p];

    // Yirtilmayi (torn store) onle: magaza tek bir cache satiri icinde kalmali.
    // x86'da satir-ici magaza atomiktir; satiri asarsa baska bir cekirdek yarim
    // yazilmis disp gorup yanlis adres hesaplayabilir.
    // Mevcut disp32 = 0 ve TEB ofseti < 0x10000 oldugundan yalnizca ALT 2 BAYTI
    // yazmak yeterli; ust 2 bayt zaten 0. Boylece 4 bayt yerine 2 bayt kaydiriyoruz
    // ve satir asma ihtimali 3/64'ten 1/64'e duser.
    const uintptr_t a = reinterpret_cast<uintptr_t>(disp_ptr);
    if ((a & 63u) > 62u) { LogPatchSkip(code, "disp cache satirini asiyor"); return; }

    DWORD old_prot = 0;
    if (!VirtualProtect(code, 16, PAGE_EXECUTE_READWRITE, &old_prot)) {
        LogPatchSkip(code, "VirtualProtect basarisiz");
        return;
    }
    *reinterpret_cast<volatile uint16_t*>(disp_ptr) =
        static_cast<uint16_t>(g_teb_slot_off);  // 1) disp'in alt 16 biti
    _mm_sfence();
    code[seg_pos] = 0x65;                                             // 2) FS -> GS
    DWORD tmp = 0;
    VirtualProtect(code, 16, old_prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), code, 16);

    static volatile LONG s_patched = 0;
    LONG n = InterlockedIncrement(&s_patched);
    if (n <= 5 || (n % 100) == 0) {
        printf("[TLS] Yama #%ld: 0x%llx artik gs:[0x%x] okuyor (bu komut bir daha fault etmez)\n",
               n, reinterpret_cast<uint64_t>(code), g_teb_slot_off);
        fflush(stdout);
    }
}

// Cozulen hedef register koduna gore CONTEXT alanina yazan yardimci fonksiyon
static void SetContextReg(PCONTEXT ctx, int reg, uint64_t value) {
    switch (reg) {
        case 0:  ctx->Rax = value; break;
        case 1:  ctx->Rcx = value; break;
        case 2:  ctx->Rdx = value; break;
        case 3:  ctx->Rbx = value; break;
        case 4:  ctx->Rsp = value; break;
        case 5:  ctx->Rbp = value; break;
        case 6:  ctx->Rsi = value; break;
        case 7:  ctx->Rdi = value; break;
        case 8:  ctx->R8  = value; break;
        case 9:  ctx->R9  = value; break;
        case 10: ctx->R10 = value; break;
        case 11: ctx->R11 = value; break;
        case 12: ctx->R12 = value; break;
        case 13: ctx->R13 = value; break;
        case 14: ctx->R14 = value; break;
        case 15: ctx->R15 = value; break;
    }
}

// VEH icinde gecen sureyi olcer (metrik cubugundaki "veh %%").
// Handler'in birden fazla cikis noktasi var; RAII ile hepsini kapsiyoruz.
// Bu fonksiyonda __try/__except yok, dolayisiyla yikici guvenle calisir.
// PLT dispatch onbellekleri + cagri sayaclari (dosya kapsaminda: "en cok
// cagrilanlar" dokumu handler disindan da okuyabilsin).
static constexpr uint32_t kPltCacheMax = 4096;
static const std::string* g_plt_fn_cache[kPltCacheMax] = {};
static const std::string* g_plt_rn_cache[kPltCacheMax] = {};
static std::atomic<uint32_t> g_plt_counts[kPltCacheMax];
static std::atomic<uint64_t> g_plt_cycles[kPltCacheMax];
// Kare suresinin nereye gittigini bulmak icin: YALNIZCA oyun thread'inde ve
// DUVAR SAATIYLE (QPC) olculen sure. CPU dongusu yetmiyor cunku bloklayan
// cagrilar (uyku/bekleme) dongu harcamaz ama kareyi yer.
static std::atomic<uint64_t> g_plt_wall[kPltCacheMax];
static DWORD                 g_game_tid = 0;

// ============================================================================
// SON HLE CAGRILARI HALKA TAMPONU
// ----------------------------------------------------------------------------
// Cokme aninda "hemen once ne calisti" sorusunu cevaplar. Normal log yeterli
// degil: gurultu filtresi her fonksiyonu 8 cagridan sonra susturuyor, bu
// yuzden cokmeden onceki gercek sira logda GORUNMUYOR.
// RSP ve donus adresini de tutuyoruz - yigin bozulmasi arastirmasinin cekirdegi
// budur: donus adresi bir komutun ORTASINA dusuyorsa, onu kimin yazdigini
// buradan geriye dogru izleyebiliyoruz.
// ============================================================================
struct PltTrace {
    const std::string* name;
    uint64_t           rsp;
    uint64_t           ret;
    DWORD              tid;
};
static constexpr uint32_t kPltTraceMax = 24;
static PltTrace              g_plt_trace[kPltTraceMax];
static std::atomic<uint32_t> g_plt_trace_pos{0};

static void DumpPltTrace(std::stringstream& out) {
    const uint32_t pos = g_plt_trace_pos.load(std::memory_order_relaxed);
    out << "\n[-] --- SON " << kPltTraceMax << " HLE CAGRISI (en yeni en altta) ---";
    for (uint32_t i = 0; i < kPltTraceMax; i++) {
        const PltTrace& e = g_plt_trace[(pos + i) % kPltTraceMax];
        if (e.name == nullptr) continue;
        out << "\n    TID=" << std::dec << e.tid << "  RSP=0x" << std::hex << e.rsp
            << "  RET=0x" << e.ret << "  " << *e.name;
    }
    out << std::dec;
}

// Saniyede on binlerce PLT cagrisi var; HANGI fonksiyonlar oldugunu bilmeden
// optimize etmek tahmindir. Periyodik olarak en sicak 15'ini dok.
extern "C" void PsemuDumpPltTop() {
    static uint32_t s_prev_n[kPltCacheMax] = {};
    static uint64_t s_prev_c[kPltCacheMax] = {};
    static uint64_t s_prev_w[kPltCacheMax] = {};
    struct Row { uint32_t idx, calls; uint64_t cyc, wall; };
    Row top[12] = {};
    uint64_t total_wall = 0;
    for (uint32_t i = 0; i < kPltCacheMax; i++) {
        const uint32_t n_now = g_plt_counts[i].load(std::memory_order_relaxed);
        const uint64_t c_now = g_plt_cycles[i].load(std::memory_order_relaxed);
        const uint64_t w_now = g_plt_wall[i].load(std::memory_order_relaxed);
        const uint32_t dn = n_now - s_prev_n[i];
        const uint64_t dc = c_now - s_prev_c[i];
        const uint64_t dw = w_now - s_prev_w[i];
        s_prev_n[i] = n_now;
        s_prev_c[i] = c_now;
        s_prev_w[i] = w_now;
        total_wall += dw;
        if (dn == 0) continue;
        // CAGRI SAYISINA gore sirala. Dongu sutunu bilgi amacli duruyor ama
        // guvenilir degil (QueryThreadCycleTime baglam degisiminde sacmaliyor);
        // oysa her cagri bir exception turu demek, yani sayim = maliyet.
        // OYUN THREAD'INDEKI DUVAR SAATINE gore sirala: kare suresini yiyen sey
        // budur (bloklayan cagrilar dongu harcamaz ama kareyi yer).
        // SIRALAMA CAGRI SAYISINA gore. Eskiden duvar saatine goreydi ama o
        // yalnizca OYUN THREAD'inde olculuyor; Astro Bot'ta CPU'yu yiyenler
        // worker thread'ler oldugu icin tablo tamamen BOS cikiyordu
        // ("toplam 0 ms"). Zaten her cagri bir exception turu, yani sayim
        // dogrudan maliyettir.
        for (int k = 0; k < 12; k++) {
            if (dn > top[k].calls) {
                for (int m = 11; m > k; m--) top[m] = top[m - 1];
                top[k] = Row{i, dn, dc, dw};
                break;
            }
        }
    }
    {
        static uint64_t pv = 0, pn = 0;
        const uint64_t v = g_vq_calls.load(std::memory_order_relaxed);
        const uint64_t nn = g_veh_nested.load(std::memory_order_relaxed);
        // Toplam PLT cagrisi = toplam Windows exception sayisi (her PLT cagrisi
        // bir access violation -> VEH turu). Yukleme suresinin ne kadarinin
        // buna gittigini kestirmek icin en onemli sayi bu.
        uint64_t total_plt = 0;
        for (uint32_t i = 0; i < kPltCacheMax; i++) {
            total_plt += g_plt_counts[i].load(std::memory_order_relaxed);
        }
        printf("[MEM] aralikta: VirtualQuery %llu | ic ice VEH %llu || BASTAN BERI toplam PLT (=exception) %llu\n",
               static_cast<unsigned long long>(v - pv),
               static_cast<unsigned long long>(nn - pn),
               static_cast<unsigned long long>(total_plt));
        pv = v;
        pn = nn;
    }
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const double ms_per_tick = 1000.0 / static_cast<double>(qf.QuadPart);
    printf("[PLT-TOP] OYUN THREAD'inde HLE icinde gecen sure (son aralik, toplam %.0f ms):\n",
           static_cast<double>(total_wall) * ms_per_tick);
    for (int k = 0; k < 12 && top[k].calls > 0; k++) {
        const std::string* rn = g_plt_rn_cache[top[k].idx];
        const std::string* fn = g_plt_fn_cache[top[k].idx];
        const char* nm = (rn && !rn->empty()) ? rn->c_str() : (fn ? fn->c_str() : "?");
        printf("   %8.1f ms  %6u cagri  %7.3f ms/cagri  PLT#%u %s\n",
               static_cast<double>(top[k].wall) * ms_per_tick, top[k].calls,
               top[k].calls ? static_cast<double>(top[k].wall) * ms_per_tick / top[k].calls : 0.0,
               top[k].idx, nm);
    }
    fflush(stdout);
}

// NOT: duvar saati YANILTICI. HLE'nin bloklayan cagrilari (sceAudioOutOutput
// pacing'i, WaitSema, vblank beklemesi) VEH'in ICINDE calisiyor; duvar saatiyle
// olcunce "VEH %450" cikiyor ama bunun cogu UYKU, CPU yanmasi degil. Bu yuzden
// QueryThreadCycleTime ile GERCEK CPU dongusu sayiyoruz: uyuyan thread dongu
// harcamaz. Boylece metrik "emulasyon katmani ne kadar CPU yiyor" sorusunu
// dogru cevaplar.
// IC ICE VEH SAYACI: HLE handler'imiz misafir bellegine dokundugunda
// (memcpy/memcmp'in SafeReadable'i, RET simulasyonu, vs.) sayfa henuz commit
// edilmemisse YENI bir access violation olusur ve VEH kendi icinden tekrar
// cagrilir. Bu ic ice tur tam bir Windows exception gidis-donusu (cekirdek
// dahil) demek ve DIS PltTimer penceresine yazilir - bu yuzden "hicbir sey
// yapmayan" __cxa_atexit bile 145.000 dongu gorunebiliyor. Sayarak dogruluyoruz.
static thread_local int t_veh_depth = 0;

namespace {
struct VehTimer {
    ULONG64 c0 = 0;
    VehTimer() {
        if (++t_veh_depth > 1) {
            g_veh_nested.fetch_add(1, std::memory_order_relaxed);
        }
        QueryThreadCycleTime(GetCurrentThread(), &c0);
    }
    ~VehTimer() {
        ULONG64 c1 = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &c1) && c1 > c0) {
            PsemuMetricAddVehCycles(c1 - c0);
        }
        --t_veh_depth;
    }
};
} // namespace

LONG WINAPI Core::SyscallExceptionFilter(EXCEPTION_POINTERS* ExceptionInfo) {
    VehTimer veh_timer;
    DWORD code = ExceptionInfo->ExceptionRecord->ExceptionCode;
    PCONTEXT ctx = ExceptionInfo->ContextRecord;

    // TANI breakpoint: utf16 donusum girisinde kaynagi dok
    if (code == EXCEPTION_BREAKPOINT && g_diag_bp_addr != 0 &&
        ctx->Rip == g_diag_bp_addr + 1) {
        // rsi = value nesnesi; kaynak string [rsi+0x10]'da (std::u16string).
        uint64_t vobj = ctx->Rsi;
        if (SafeReadable(reinterpret_cast<void*>(vobj + 0x10), 0x18)) {
            uint8_t* su = reinterpret_cast<uint8_t*>(vobj + 0x10);
            // libc++ u16string: SSO ise dusuk bit set; degilse [ptr,size,cap].
            uint64_t f0 = *reinterpret_cast<uint64_t*>(su);
            uint64_t sz = *reinterpret_cast<uint64_t*>(su + 8);
            const uint16_t* s16; size_t n16;
            if (f0 & 1) { // long form: size <<1 |1, ptr@+16
                n16 = sz; // yaklasik
                s16 = *reinterpret_cast<const uint16_t**>(su + 16);
            } else {      // short (SSO): inline, ilk bayt size<<1
                n16 = (f0 & 0xff) >> 1;
                s16 = reinterpret_cast<const uint16_t*>(su + 1);
            }
            // Surrogate ara
            bool bad = false;
            if (s16 && n16 < 4096 && SafeReadable(s16, (n16 ? n16 : 1) * 2)) {
                for (size_t i = 0; i < n16; i++)
                    if (s16[i] >= 0xD800 && s16[i] <= 0xDBFF) { bad = true; break; }
            }
            if (bad) {
                std::stringstream ds;
                ds << "[UTF16-TANI] BOZUK kaynak (uzunluk=" << n16 << "): ";
                for (size_t i = 0; i < n16 && i < 40; i++)
                    ds << std::hex << std::setw(4) << std::setfill('0') << s16[i] << " ";
                LOG_ERROR(ds.str());
                // ASCII yorumu
                std::stringstream as; as << "[UTF16-TANI] ascii: ";
                for (size_t i = 0; i < n16 && i < 60; i++) {
                    uint16_t c = s16[i];
                    as << (char)((c >= 32 && c < 127) ? c : '.');
                }
                LOG_ERROR(as.str());
            }
        }
        // orijinal bayti geri koy, RIP'i geri sar, single-step ile re-arm
        *reinterpret_cast<uint8_t*>(g_diag_bp_addr) = g_diag_bp_orig;
        ctx->Rip = g_diag_bp_addr;
        ctx->EFlags |= 0x100; // TF
        g_diag_bp_pending = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // .init_array izleyicisi: "call rax" yerine konan 0xCC
    // NOT: Windows EXCEPTION_BREAKPOINT'te ctx->Rip'i INT3'un USTUNDE verir
    // (asagidaki syscall yolu da bu yuzden "Rip += 1" yapiyor).
    if (code == EXCEPTION_BREAKPOINT && g_initcall_addr != 0 &&
        (ctx->Rip == g_initcall_addr || ctx->Rip == g_initcall_addr + 1)) {
        g_initcall_last = ctx->Rax - g_base_addr;
        g_initcall_cursor = ctx->Rbx - g_base_addr;
        uint64_t n = ++g_initcall_n;
        // Varsayilan olarak SESSIZ: VEH icinden her girdide log yazmak
        // logger kilidi uzerinden kilitlenmeye yol aciyor. Sayaclar cokme
        // dokumunde basiliyor; ayrintili iz icin PSEMU_INIT_TRACE_LOG=1.
        // r15/r14 SysV'de callee-saved. module_start bunlarda argv/argc
        // tasiyor; hangi ilklendiricinin bozdugunu yakalamak icin her
        // girdide degisimi kontrol ediyoruz (tek karsilastirma, ucuz).
        if (n == 1) {
            g_initcall_r15 = ctx->R15;
            g_initcall_r14 = ctx->R14;
            // Statik init'in ILK girdisinde argv hala saglam mi? Bu, "argv'yi
            // ilklendiriciler mi bozuyor yoksa daha once mi bozuluyor"
            // sorusunu tek olcumde ayiriyor.
            std::stringstream as;
            as << "[INIT-ARGV] ilk girdide R15=0x" << std::hex << ctx->R15
               << " beklenen argv=0x" << g_expected_argv << "  -> "
               << (ctx->R15 == g_expected_argv ? "SAGLAM" : "ZATEN BOZUK")
               << " | R14(argc)=0x" << ctx->R14;
            LOG_ERROR(as.str());
        } else if (ctx->R15 != g_initcall_r15 || ctx->R14 != g_initcall_r14) {
            std::stringstream cs;
            cs << "[INIT-CLOBBER] #" << std::dec << (n - 1) << " (RVA 0x"
               << std::hex << g_initcall_prev << ") callee-saved bozdu: "
               << "R15 0x" << g_initcall_r15 << " -> 0x" << ctx->R15
               << " | R14 0x" << g_initcall_r14 << " -> 0x" << ctx->R14;
            LOG_ERROR(cs.str());
            g_initcall_r15 = ctx->R15;
            g_initcall_r14 = ctx->R14;
        }
        g_initcall_prev = g_initcall_last;

        if (g_initcall_verbose) {
            std::stringstream is;
            is << "[INIT#" << std::dec << n << "] RVA 0x" << std::hex << g_initcall_last
               << "  (imlec RVA 0x" << g_initcall_cursor << ")";
            LOG_INFO(is.str());
        }
        // "call rax"i elde canlandir
        ctx->Rsp -= 8;
        *reinterpret_cast<uint64_t*>(ctx->Rsp) = g_initcall_next;
        ctx->Rip = ctx->Rax;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Prologue breakpoint'i: "push rbp" yerine konan 0xCC
    if (code == EXCEPTION_BREAKPOINT && g_bp_count > 0) {
        for (int i = 0; i < g_bp_count; i++) {
            if (ctx->Rip != g_bp_addr[i] && ctx->Rip != g_bp_addr[i] + 1) continue;
            if (g_bp_hits[i]++ < kBpLogLimit) {
                uint64_t ret = 0;
                if (SafeReadable(reinterpret_cast<void*>(ctx->Rsp), 8))
                    ret = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                std::stringstream bs;
                bs << "[BP] RVA 0x" << std::hex << (g_bp_addr[i] - g_base_addr)
                   << " #" << std::dec << g_bp_hits[i] << std::hex
                   << "  CAGIRAN RVA 0x" << (ret - g_base_addr)
                   << "\n     RDI=0x" << ctx->Rdi << " RSI=0x" << ctx->Rsi
                   << " RDX=0x" << ctx->Rdx << " RCX=0x" << ctx->Rcx
                   << " R8=0x" << ctx->R8 << " R9=0x" << ctx->R9
                   << " RAX=0x" << ctx->Rax
                   << "\n     [callee-saved] RBX=0x" << ctx->Rbx << " RBP=0x" << ctx->Rbp
                   << " R12=0x" << ctx->R12 << " R13=0x" << ctx->R13
                   << " R14=0x" << ctx->R14 << " R15=0x" << ctx->R15
                   << " RSP=0x" << ctx->Rsp;
                // SysV'de 7. argumandan itibaren YIGINDA gelir: [RSP+8]'den
                // baslar (RSP'de donus adresi durur). Cok argumanli
                // fonksiyonlarda tek basina register dokumu yetmiyor.
                bs << "\n     [yigin arg]";
                for (int k = 1; k <= 8; k++) {
                    uint64_t* sp = reinterpret_cast<uint64_t*>(ctx->Rsp + k * 8);
                    if (!SafeReadable(sp, 8)) break;
                    bs << " +" << std::dec << (k * 8) << "=0x" << std::hex << *sp;
                }
                // Isaretci gibi duran argumanlarin ICERIGINI de dok: "yapi
                // geldi ama alanlari bos mu?" sorusunu ancak boyle
                // cevaplayabiliyoruz.
                {
                    const char* nm[3] = {"RDI", "RSI", "RDX"};
                    uint64_t rv[3] = {ctx->Rdi, ctx->Rsi, ctx->Rdx};
                    for (int a = 0; a < 3; a++) {
                        uint64_t* q = reinterpret_cast<uint64_t*>(rv[a]);
                        if (rv[a] < 0x10000 || !SafeReadable(q, 64)) continue;
                        bs << "\n     [" << nm[a] << "]->";
                        for (int k = 0; k < 8; k++) bs << " 0x" << std::hex << q[k];
                    }
                }
                LOG_INFO(bs.str());
            }
            // "push rbp"yi elde canlandir
            ctx->Rsp -= 8;
            *reinterpret_cast<uint64_t*>(ctx->Rsp) = ctx->Rbp;
            ctx->Rip = g_bp_addr[i] + 1;
            if (g_watchw_addr != 0 && !g_watchw_armed) {
                g_watchw_armed = true;
                ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
                ctx->Dr0 = g_watchw_addr;
                ctx->Dr7 = 0xD0001; // L0 + yazma + 4 bayt
                ctx->Dr6 = 0;
                std::stringstream ws;
                ws << "[WATCH-W] donanim yazma izlemesi kuruldu: 0x"
                   << std::hex << g_watchw_addr;
                LOG_ERROR(ws.str());
            }
            if (g_canary_bp != 0 && g_bp_addr[i] == g_canary_bp && g_canary_addr == 0) {
                // "push rbp"yi canlandirdik; siradaki "push r15" buraya yazacak
                g_canary_addr = ctx->Rsp - 8;
                g_canary_val  = ctx->R15;
                std::stringstream cs;
                cs << "[KANARYA] kuruldu: yigin gozu 0x" << std::hex << g_canary_addr
                   << " beklenen deger 0x" << g_canary_val;
                LOG_ERROR(cs.str());
            }
            if (g_trace_from != 0 && g_bp_addr[i] == g_trace_from && !g_trace_active) {
                g_trace_active = true;
                ctx->EFlags |= 0x100; // TF
                LOG_INFO("[TRACE] tek adim izleme basladi.");
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // Donanim yazma izlemesi tetiklendi mi? (DR0 -> Dr6 bit0)
    if (code == EXCEPTION_SINGLE_STEP && g_watchw_armed && (ctx->Dr6 & 1)) {
        if (g_watchw_hits++ < 10) {
            std::stringstream ws;
            ws << "[WATCH-W] 0x" << std::hex << g_watchw_addr
               << " adresine YAZAN komut: RVA 0x" << (ctx->Rip - g_base_addr)
               << "  | yeni deger 0x";
            if (SafeReadable(reinterpret_cast<void*>(g_watchw_addr), 4))
                ws << *reinterpret_cast<uint32_t*>(g_watchw_addr);
            ws << " | RDI=0x" << ctx->Rdi << " RSI=0x" << ctx->Rsi
               << " RCX=0x" << ctx->Rcx << " RDX=0x" << ctx->Rdx;
            LOG_ERROR(ws.str());
        }
        ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Tek adim izleme: sadece RIP'i kaydet, TF'i kurulu tut.
    if (code == EXCEPTION_SINGLE_STEP && g_trace_active) {
        g_trace_ring[g_trace_pos++ % kTraceRing] = ctx->Rip;
        if (g_expected_argv != 0 && ctx->R15 != g_r15_prev &&
            (g_r15_prev == g_expected_argv || ctx->R15 == g_expected_argv)) {
            R15Event& e = g_r15_ring[g_r15_pos++ % kR15Ring];
            e.rip = ctx->Rip; e.from = g_r15_prev; e.to = ctx->R15;
        }
        g_r15_prev = ctx->R15;
        if (++g_trace_steps >= g_trace_max) {
            g_trace_active = false;
            ctx->EFlags &= ~0x100;
        } else {
            ctx->EFlags |= 0x100;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP && g_diag_bp_pending) {
        g_diag_bp_pending = false;
        ctx->EFlags &= ~0x100;
        *reinterpret_cast<uint8_t*>(g_diag_bp_addr) = 0xCC; // re-arm
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Sadece EXCEPTION_BREAKPOINT (INT 3 - 0xCC) hatalarini yakalayacagiz
    if (code == EXCEPTION_BREAKPOINT) {

        // System V ABI'ye gore argumanlari al:
        uint64_t syscall_id = ctx->Rax;
        uint64_t arg1 = ctx->Rdi;
        uint64_t arg2 = ctx->Rsi;
        uint64_t arg3 = ctx->Rdx;
        uint64_t arg4 = ctx->R10; 
        uint64_t arg5 = ctx->R8;
        uint64_t arg6 = ctx->R9;

        // Syscall isleyicisini (HLE Stub) cagir
        uint64_t result = SyscallManager::HandleSyscall(syscall_id, arg1, arg2, arg3, arg4, arg5, arg6);

        // Sonucu RAX'a yaz
        ctx->Rax = result;
        ctx->Rip += 1;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // ================================================================
    // TANI: Watchpoint - Single-Step ile gecikmeli yeniden silahlandirma
    // ================================================================
    // PAGE_GUARD, OS tarafindan istisna teslim edilmeden ONCE otomatik
    // kaldirilir. Eger guard'i AYNI handler cagrisinda hemen geri
    // takarsak, EXCEPTION_CONTINUE_EXECUTION komutu YENIDEN denedigi anda
    // guard hala orada olur ve sonsuz dongu olusur. Bunun yerine: guard
    // tetiklendiginde TF (trap flag) set edip komutun (guard'siz) calismasina
    // izin veriyoruz; hemen ardindan gelen SINGLE_STEP istisnasinda guard'i
    // guvenle yeniden takiyoruz.
    if (code == EXCEPTION_SINGLE_STEP && g_watch_rearm_pending) {
        g_watch_rearm_pending = false;
        ctx->EFlags &= ~0x100; // TF temizle
        ArmWatchpoint();
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // ================================================================
    // TANI: Watchpoint (PAGE_GUARD) tetiklendi
    // ================================================================
    // STATUS_GUARD_PAGE_VIOLATION (0x80000001) sayfa-genelinde tetiklenir;
    // erisim adresini hedef global ile karsilastirip gurultuyu eliyoruz.
    if (code == 0x80000001 && ExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        uint64_t wp_type = ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        uint64_t wp_addr = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];

        if (g_watch_target != 0 && wp_addr >= g_watch_target && wp_addr < g_watch_target + 8) {
            g_watch_hits++;
            if (g_watch_hits <= kWatchHitLimit) {
                std::stringstream wss;
                wss << "[WATCHPOINT-HIT #" << g_watch_hits << "] "
                    << (wp_type == 1 ? "WRITE" : "READ") << " @ 0x" << std::hex << wp_addr
                    << " | RIP: 0x" << ctx->Rip;
                if (IsInModuleRange(ctx->Rip)) {
                    wss << " (RVA: 0x" << (ctx->Rip - g_base_addr) << ")";
                }
                wss << std::dec
                    << " | RAX=0x" << std::hex << ctx->Rax << " RBX=0x" << ctx->Rbx
                    << " RCX=0x" << ctx->Rcx << " RDX=0x" << ctx->Rdx
                    << " RSI=0x" << ctx->Rsi << " RDI=0x" << ctx->Rdi
                    << " RSP=0x" << ctx->Rsp << " RBP=0x" << ctx->Rbp << std::dec;
                LOG_ERROR(wss.str());

                // Eger bu bir YAZMA ise, yazilan degeri de gorelim (komut henuz
                // calismadi, bu yuzden hedef adresteki eski degeri degil, hangi
                // registerin yazilacagini logluyoruz - RIP'teki komutu okumak
                // gerekiyor ama bunun icin de basit bir hex dump yeterli).
                if (wp_type == 1 && IsInModuleRange(ctx->Rip) && SafeReadable(reinterpret_cast<void*>(ctx->Rip), 8)) {
                    const uint8_t* wb = reinterpret_cast<const uint8_t*>(ctx->Rip);
                    std::stringstream ib;
                    ib << "[WATCHPOINT-HIT] Yazma komutu baytlari: ";
                    for (int wi = 0; wi < 8; wi++) {
                        char buf[4];
                        snprintf(buf, sizeof(buf), "%02X ", wb[wi]);
                        ib << buf;
                    }
                    LOG_ERROR(ib.str());
                }
            } else if (g_watch_hits == kWatchHitLimit + 1) {
                LOG_ERROR("[WATCHPOINT] Hit siniri asildi, daha fazla loglama yapilmayacak (izleme devam ediyor).");
            }
        }

        // Guard'i HEMEN geri takmiyoruz (sonsuz donguye yol acar). Bunun
        // yerine TF (trap flag) ile bir sonraki komutta SINGLE_STEP
        // istisnasi tetikleyip, guard'i orada guvenle yeniden kuruyoruz.
        ctx->EFlags |= 0x100;
        g_watch_rearm_pending = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // ================================================================
    // YARIS-TASMA GENEL HANDLER (tip-kayit fonksiyonu 0x2dfff0)
    // ================================================================
    // Tip-kayit 4-slot tabloya bazen 5. kaydi yapmaya calisir; cmova ile base
    // register (rbx/rdx) NULL'a duser ve [base+disp] NULL-yakini erisim (OKUMA
    // veya YAZMA) cokerdi. Onceki cozum her taÅŸma komutunu tek tek atliyordu
    // (whack-a-mole; her timing degisiminde yeni RVA â€” Vulkan wiring sonrasi
    // 0x2e08aa gibi). GENEL COZUM: bu fonksiyonda (0x2dfff0..0x2e0900)
    // NULL-base fault olunca, NULL register'i (rbx/rdx) sifirlanmis bir DUMMY
    // buffer'a yonlendirip komutu YENIDEN CALISTIR. Boylece tum taÅŸma erisimleri
    // (5. kayit) zararsizca dummy'ye gider, ilk 4 gecerli kayit tabloda kalir,
    // fonksiyon normal tamamlanir. Instruction atlama / whack-a-mole YOK.
    // ================================================================
    // MISAFIR TUZAGI: int 0x41
    // ================================================================
    // PS5 kodu hata durumunda "int 0x41" isletir (assert/abort tuzagi).
    // Windows bunu genel koruma hatasi olarak teslim eder ve hata adresi
    // 0xFFFFFFFFFFFFFFFF gorunur - yani BELLEK HATASI gibi gorunur, oysa
    // degildir. Astro Bot'ta olculen kalip:
    //     test eax, eax ; je +2 ; int 0x41   <- donus degeri sifir degilse tuzak
    // Bunu ayirt edip ANLASILIR sekilde raporluyoruz; aksi halde saatlerce
    // yanlis yerde (bellek bozulmasinda) aranir.
    //
    // Varsayilan olarak ATLIYORUZ (RIP += 2): tuzak oyunun kendi "burada
    // durmaliyim" karari; emulatorde ilerlemeye devam edip bir sonraki gercek
    // eksigi gormek istiyoruz. Kapatmak icin: PSEMU_TRAP_FATAL=1
    // BOS ISARETCIYE YAZMAYI ATLA (PSEMU_SKIP_NULL_STORE=1)
    if (g_skip_null_store && code == EXCEPTION_ACCESS_VIOLATION &&
        ExceptionInfo->ExceptionRecord->NumberParameters >= 2 &&
        ExceptionInfo->ExceptionRecord->ExceptionInformation[0] == 1 &&
        ExceptionInfo->ExceptionRecord->ExceptionInformation[1] < 0x100000 &&
        ctx->Rip >= g_base_addr && ctx->Rip < g_base_addr + g_module_size &&
        SafeReadable(reinterpret_cast<void*>(ctx->Rip), 16)) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ctx->Rip);
        size_t len = DecodeStoreLen(p);
        if (len != 0) {
            if (g_skip_hits++ < 20) {
                std::stringstream ks;
                ks << "[NULL-STORE] RVA 0x" << std::hex << (ctx->Rip - g_base_addr)
                   << " adres 0x" << ExceptionInfo->ExceptionRecord->ExceptionInformation[1]
                   << " (" << std::dec << len << " bayt) ATLANDI";
                LOG_ERROR(ks.str());
            }
            ctx->Rip += len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // KOD SAYFASINA YAZMA: kim ve hangi thread? (PSEMU_PROTECT_CODE=1)
    if (g_protect_code && code == EXCEPTION_ACCESS_VIOLATION &&
        ExceptionInfo->ExceptionRecord->NumberParameters >= 2 &&
        ExceptionInfo->ExceptionRecord->ExceptionInformation[0] == 1) { // 1 = YAZMA
        uint64_t fault = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        if (fault >= g_base_addr && fault < g_base_addr + g_text_size) {
            if (g_codew_hits++ < 24) {
                std::stringstream ws;
                ws << "[KOD-YAZMA] RVA 0x" << std::hex << (fault - g_base_addr)
                   << " adresine YAZILDI | TID=" << std::dec << GetCurrentThreadId()
                   << " | yazan RIP=0x" << std::hex << ctx->Rip;
                if (ctx->Rip >= g_base_addr && ctx->Rip < g_base_addr + g_module_size)
                    ws << " (misafir RVA 0x" << (ctx->Rip - g_base_addr) << ")";
                else
                    ws << " (MISAFIR DISI - emulator/host kodu)";
                ws << " RDI=0x" << ctx->Rdi << " RSI=0x" << ctx->Rsi
                   << " RCX=0x" << ctx->Rcx;
                LOG_ERROR(ws.str());
            }
            // Sayfayi yazilabilir yap ve devam et (oyun durmasin).
            SYSTEM_INFO si; GetSystemInfo(&si);
            void* page = reinterpret_cast<void*>(fault & ~(uint64_t)(si.dwPageSize - 1));
            DWORD oldp = 0;
            VirtualProtect(page, si.dwPageSize, PAGE_EXECUTE_READWRITE, &oldp);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ExceptionInfo->ExceptionRecord->NumberParameters >= 2 &&
        ExceptionInfo->ExceptionRecord->ExceptionInformation[1] == ~0ull) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ctx->Rip);
        if (SafeReadable(p, 2) && p[0] == 0xCD && p[1] == 0x41) {
            static const bool s_fatal = [] {
                const char* e = getenv("PSEMU_TRAP_FATAL");
                return e != nullptr && e[0] != '0';
            }();
            static std::atomic<uint64_t> s_traps{0};
            const uint64_t tn = s_traps.fetch_add(1) + 1;
            if (tn <= 12 || (tn % 500ull) == 0) {
                std::stringstream ts;
                ts << "[GUEST-TRAP] int 0x41 (oyunun kendi hata tuzagi) #" << tn << " @ RVA 0x"
                   << std::hex << (ctx->Rip - g_base_addr) << " | RAX=0x" << ctx->Rax
                   << " RDI=0x" << ctx->Rdi << " RSI=0x" << ctx->Rsi << std::dec
                   << (s_fatal ? "  -> OLUMCUL (PSEMU_TRAP_FATAL=1)" : "  -> atlandi");
                LOG_ERROR(ts.str());
            }
            if (!s_fatal) {
                ctx->Rip += 2; // tuzagi gec
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    // OYUNA OZEL: bu RVA araligi yalnizca Dreaming Sarah'in binary'sinde
    // tip-kayit fonksiyonudur. Baska bir oyunda ayni adreste bambaska kod olur
    // ve asagidaki "RBX/RDX'i dummy'ye yonlendir" duzeltmesi sessizce bellegi
    // bozar. Bu yuzden profile bagli (bkz. game_profile.h).
    if (Game::Current().quirk_c2_type_registration_overflow &&
        code == EXCEPTION_ACCESS_VIOLATION &&
        ctx->Rip >= g_base_addr + 0x2dfff0 && ctx->Rip < g_base_addr + 0x2e0900) {
        uint64_t fault = ExceptionInfo->ExceptionRecord->NumberParameters >= 2 ?
                         ExceptionInfo->ExceptionRecord->ExceptionInformation[1] : ~0ull;
        if (fault < 0x1000) {
            static thread_local uint8_t* t_dummy_buf = nullptr;
            if (t_dummy_buf == nullptr) {
                t_dummy_buf = reinterpret_cast<uint8_t*>(
                    VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            }
            uint64_t dummy = reinterpret_cast<uint64_t>(t_dummy_buf);
            bool fixed = false;
            if (ctx->Rbx < 0x1000) { ctx->Rbx = dummy; fixed = true; }
            if (ctx->Rdx < 0x1000) { ctx->Rdx = dummy; fixed = true; }
            if (fixed) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    // OYUNA OZEL: RVA 0x1654f8 (mov esi, [rcx + 0x120]) texture metadata
    // okumasi. Adres Dreaming Sarah'a ait; baska oyunda RIP'i 0x165508'e
    // zorlamak keyfi bir yere atlamak demektir.
    if (Game::Current().quirk_texture_meta_recover && code == EXCEPTION_ACCESS_VIOLATION &&
        ctx->Rip == g_base_addr + 0x1654f8) {
        ctx->Rsi = 1;
        ctx->Rip = g_base_addr + 0x165508; // Jump past jz check to force main menu scene transition!
        LOG_INFO("[VEH-RECOVER] Force-completed texture load at RVA 0x1654f8 -> transitioning scene to main menu!");
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Eger Syscall disinda baska bir cokme yasandiysa detayli register dokumu yap.
    //
    // PERFORMANS: bu handler saniyede ~100.000 kez cagriliyor (PLT dispatch'in
    // tamami buradan geciyor) ve asagidaki yollarin NEREDEYSE HEPSI erken
    // return ediyor - yani tani metni hic kullanilmiyor. std::stringstream
    // kurulumu ucuz DEGIL (locale + stringbuf + heap). Bu yuzden akisi tembel
    // yaptik: metin ancak gercekten cokme raporlayacaksak insa edilir.
    std::unique_ptr<std::stringstream> ss_ptr;
    auto ss_init = [&]() -> std::stringstream& {
        if (!ss_ptr) {
            ss_ptr = std::make_unique<std::stringstream>();
            *ss_ptr << "CRASH yakalandi! Kod: 0x" << std::hex << code
                    << " | RIP: 0x" << ctx->Rip;
            if (ctx->Rip >= g_base_addr && ctx->Rip < g_base_addr + g_module_size) {
                *ss_ptr << " (RVA 0x" << (ctx->Rip - g_base_addr) << ")";
            }
            if (code == 0xC00000FD) {
                *ss_ptr << " [STACK OVERFLOW]";
            }
            // RIP'teki baytlari HER cokme turunde dok. Eskiden yalnizca erisim
            // ihlallerinde basiliyordu; oysa "gecersiz komut" (0xC000001D)
            // hatalarinda bu bilgi daha da kritik - bellekteki kodun dosyadaki
            // ile ayni olup olmadigini ancak boyle karsilastirabiliyoruz.
            if (ctx->Rip >= g_base_addr && ctx->Rip < g_base_addr + g_module_size &&
                SafeReadable(reinterpret_cast<void*>(ctx->Rip), 16)) {
                const uint8_t* rb = reinterpret_cast<const uint8_t*>(ctx->Rip);
                *ss_ptr << "\n[-] RIP baytlari (RVA 0x" << std::hex << (ctx->Rip - g_base_addr)
                        << "): ";
                for (int i = 0; i < 16; i++) {
                    char b[4];
                    snprintf(b, sizeof(b), "%02X ", rb[i]);
                    *ss_ptr << b;
                }
                *ss_ptr << std::dec;
            }
            // ISTEGE BAGLI BAYT DOKUMU: PSEMU_DUMP_RVA=0xe3810,0x24ae30
            // Bellekteki kodun dosyadakiyle ayni olup olmadigini KESIN
            // karsilastirmak icin. "Kod uzerine mi yazildi?" sorusunu
            // tahminle degil olcumle kapatiyoruz.
            if (const char* dr = std::getenv("PSEMU_DUMP_RVA")) {
                const char* s = dr;
                for (int k = 0; k < 6 && *s; k++) {
                    char* end = nullptr;
                    uint64_t rva = std::strtoull(s, &end, 0);
                    if (end == s) break;
                    uint8_t* p = reinterpret_cast<uint8_t*>(g_base_addr + rva);
                    if (SafeReadable(p, 16)) {
                        *ss_ptr << "\n[-] [DUMP] RVA 0x" << std::hex << rva << ": ";
                        for (int i = 0; i < 16; i++) {
                            char b[4];
                            snprintf(b, sizeof(b), "%02X ", p[i]);
                            *ss_ptr << b;
                        }
                        *ss_ptr << std::dec;
                    }
                    s = (*end == ',') ? end + 1 : end;
                }
            }
            // CAGIRAN: vahsi dallanmalarda cokme adresi degil, ORAYA KIMIN
            // gonderdigi onemli. [RSP] tipik olarak donus adresidir; modul
            // icindeyse RVA olarak gosteriyoruz.
            if (SafeReadable(reinterpret_cast<void*>(ctx->Rsp), 8)) {
                const uint64_t ret = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                *ss_ptr << "\n[-] [RSP] = 0x" << std::hex << ret;
                if (ret >= g_base_addr && ret < g_base_addr + g_module_size) {
                    *ss_ptr << "  (RVA 0x" << (ret - g_base_addr) << ")  <- muhtemel cagiran";
                } else {
                    *ss_ptr << "  (modul DISI)";
                }
                *ss_ptr << std::dec;
            }
            DumpPltTrace(*ss_ptr);
        }
        return *ss_ptr;
    };

    // Access Violation ise hangi adrese erisilmeye calisildigini goster
    if (code == 0xC0000005 && ExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        uint64_t access_type = ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        uint64_t access_addr = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];

        // ================================================================
        // GENEL BELLEK OTOMATIK SAYFA COMMITTER (%100 COKME KORUMASI)
        // ================================================================
        // Oyun herhangi bir adrese (DirectMemory veya genel bellek havuzu)
        // READ/WRITE erisimi yaparken sayfa commit edilmemis ise (0xC0000005),
        // sayfayi aninda commit edip calismaya devam et.
        if (access_type == 0 || access_type == 1) { // 0=READ, 1=WRITE
            if (access_addr >= 0x10000ULL && access_addr < 0x7FFFFFFFFFFFULL) {
                bool handled = false;
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery(reinterpret_cast<void*>(access_addr), &mbi, sizeof(mbi)) != 0 &&
                    mbi.State == MEM_COMMIT) {
                    // Sayfa ZATEN committed ama yazilamiyor (or. Kyty PageManager
                    // write-tracking icin PAGE_READONLY yapmis). VirtualAlloc(MEM_COMMIT)
                    // zaten-committed sayfada protection'i guvenilir DEGISTIRMEZ, o yuzden
                    // yaziyi dusurmek icin ACIKCA VirtualProtect ile RW yapiyoruz. Aksi
                    // halde oyunun vertex-buffer descriptor (V#) yazisi dusmuyor, tablo
                    // sifir kaliyor ve sprite/yazi render olmuyordu (fmt=0 bug'inin koku).
                    const bool writable =
                        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                                        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
                    if (access_type == 1 && !writable) {
                        uint64_t pg = access_addr & ~0xFFFULL; // 4KB
                        DWORD old_p = 0;
                        if (VirtualProtect(reinterpret_cast<void*>(pg), 0x1000,
                                           PAGE_EXECUTE_READWRITE, &old_p) != 0) {
                            handled = true;
                            PsemuMarkCpuModified(pg, 0x1000);
                            static volatile LONG s_ro_fix = 0;
                            LONG n = InterlockedIncrement(&s_ro_fix);
                            if (n <= 12) LOG_INFO("[MEM-RO->RW] readonly guest sayfasi RW yapildi @ 0x" +
                                [](uint64_t v){ std::stringstream x; x<<std::hex<<v; return x.str(); }(access_addr));
                        }
                    } else if (writable) {
                        handled = true; // zaten yazilabilir; fault baska sebep degil, devam
                    }
                }
                if (!handled) {
                    uint64_t page_base = access_addr & ~0xFFFFULL; // 64KB hizalama
                    void* p = VirtualAlloc(reinterpret_cast<void*>(page_base), 65536, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    if (p != nullptr) {
                        static volatile LONG s_auto_commits = 0;
                        LONG n = InterlockedIncrement(&s_auto_commits);
                        if (n <= 10 || (n % 100) == 0) {
                            LOG_INFO("[MEMORY-HLE] Otomatik Sayfa Commit #" + std::to_string(n) +
                                     " @ 0x" + [](uint64_t v){ std::stringstream x; x<<std::hex<<v; return x.str(); }(access_addr));
                        }
                        handled = true;
                    }
                }
                if (handled) {
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        // ================================================================
        // HOTFIX: FS: Segment Override (TLS) Erisimi - Genellenmis Yakalayici
        // ================================================================
        // Windows'ta FS segmenti PS4/PS5 ELF'inin bekledigi Linux-tarzi TLS
        // tanimina sahip olmadigindan, "mov reg, fs:[disp]" komutlari Access
        // Violation ile duser. Tek bir sabit RIP yerine, cokme anindaki RIP'te
        // gercekten boyle bir komut olup olmadigini calisma zamaninda cozup
        // ELF'teki TUM fs: erisimlerini tek mekanizmayla yakaliyoruz.
        //
        // ONEMLI: Bu sadece READ/WRITE tipi ihlallerde denenir (access_type 0/1).
        // EXEC ihlallerinde (ornegin PLT-hook sentinel araligina jmp) ctx->Rip
        // ZATEN haritalanmamis hedef adresin kendisidir (access_addr ile ayni);
        // orada "mov reg, fs:[disp]" aramak icin IsBadReadPtr(ctx->Rip, 16)
        // cagirmak o haritalanmamis adresi tekrar probe eder, bu da YENI bir
        // access violation dogurup VEH'in (oncelik 1 ile kayitli oldugumuz icin)
        // KENDI KENDINE recursive olarak yeniden girmesine yol acar - bu da
        // ntdll'in ic register durumunu "oyun register'i" sanip yanlis RET
        // adresleriyle sahte hatalar uretmemize sebep oluyordu.
        if (access_type == 0 || access_type == 1) {
            const uint8_t* code = reinterpret_cast<const uint8_t*>(ctx->Rip);
            FsMovInfo info;
            if (SafeReadable(code, 16) && DecodeFsMov(code, info)) {
                // Her thread KENDI TLS blogunu kullanmali (bkz.
                // GetThreadTlsBase): paylasilan blok, kilitsiz
                // thread-local allocator listelerini bozuyordu.
                uint64_t tls_tp = GetThreadTlsBase();

                // Yama ara durumu: disp32'yi yazdik ama segment baytini henuz
                // GS yapmadik (ya da baska bir cekirdek tam o anda calisti).
                // Komut hala fs: oldugu icin buraya duser; niyet edilen deger
                // tp'nin KENDISI, tp+ofset degil.
                uint64_t value = (g_teb_slot_off != 0 &&
                                  static_cast<uint32_t>(info.disp) == g_teb_slot_off)
                                     ? tls_tp
                                     : tls_tp + info.disp;

                // Bu komutu kalici olarak gs:[TEB slot] okuyacak sekilde yamala;
                // basarili olursa bir daha buraya hic dusmez.
                TryPatchFsMov(const_cast<uint8_t*>(code), info);

                // OLCUM: her fs: erisimi tam bir exception gidis-donusu demek
                // (~birkac mikrosaniye). Donanim FS_BASE calisiyorsa bu sayac
                // thread basina ~1'de kalmali; binlere ciktiysa darbogaz burasi.
                PsemuMetricAddTlsFault();

                // Gurultu filtresi: her fs: erisim adresini sadece birkac kez logla.
                // Harita aramasi sicak yolda maliyet: ilk 5000 fault'tan sonra
                // haritaya hic dokunmuyoruz. Harita ayrica thread_local -
                // eski paylasimli hali kilitsizdi ve threadler arasi yaris iceriyordu.
                static std::atomic<uint64_t> s_tls_total{0};
                if (s_tls_total.fetch_add(1, std::memory_order_relaxed) < 5000) {
                    static thread_local std::map<uint64_t, int> s_tls_seen;
                    int n = ++s_tls_seen[ctx->Rip];
                    if (n <= 2) {
                        printf("[TLS-HOTFIX] FS: erisimi @ 0x%llx (disp=0x%x, tp=0x%llx) -> reg#%d = 0x%llx%s\n",
                               ctx->Rip, info.disp, tls_tp, info.dest_reg, value,
                               (n == 2) ? "  [bu adres bundan sonra susturuluyor]" : "");
                        fflush(stdout);
                    }
                }

                SetContextReg(ctx, info.dest_reg, value);
                ctx->Rip += info.instr_len;

                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        
        // ================================================================
        // PRX Hook tespiti! (Execute violation at 0x10000000000+)
        // ================================================================
        if (access_addr >= 0x10000000000ULL && access_addr < 0x10000010000ULL) {
            g_last_activity = GetTickCount64(); // hang watchdog icin aktivite isareti
            PsemuMetricAddPltCall();
            uint32_t plt_index = static_cast<uint32_t>(access_addr - 0x10000000000ULL);

            // ============================================================
            // HIZ: isim cozumlemesi plt_index basina BIR KEZ (onbellek).
            // Eski hali HER cagride: "UNKNOWN_PLT_N" string insasi (heap
            // alloc) + g_plt_names map aramasi + string kopyasi +
            // g_nid_to_name map aramasi (string karsilastirmalari) + kopya.
            // Oyun menude ~9 MILYON PLT cagrisi yapiyor (cagri basina
            // ~7.7us -> ~70s overhead = "donma"). Bu maliyeti sifirliyoruz;
            // isimler map'lerde sabit durdugu icin pointer'lari onbellekleriz.
            // ============================================================
            // TANI: handler'in ICINDE gecen sure vs cagri basina toplam sure.
            // inside << toplam ise darbogaz Windows exception (AV->VEH) round
            // trip'idir ve handler'i optimize etmek FAYDASIZ; cozum PLT/GOT
            // slotlarini native stub'a yamamaktir (exception hic olusmaz).
            // Sadece PLT#173 (memset) olcuyoruz: bloklamayan, onemsiz bir is.
            // WaitSema/MutexLock gibi handler'lar mesru sekilde BEKLEDIGI icin
            // genel toplam yanilticiydi (ve cok-thread topluyordu).
            // Her PLT indeksi icin GERCEK CPU dongusu (QueryThreadCycleTime:
            // bloklanan/uyuyan sure sayilmaz, sadece yakilan CPU). Boylece
            // "en cok cagrilan" degil "en cok CPU yiyen" fonksiyonu goruyoruz.
            struct PltTimer {
                ULONG64       c0;
                LARGE_INTEGER w0;
                uint32_t      idx;
                bool          is_game;
                explicit PltTimer(uint32_t i) : c0(0), idx(i) {
                    is_game = (g_game_tid != 0 && GetCurrentThreadId() == g_game_tid);
                    QueryThreadCycleTime(GetCurrentThread(), &c0);
                    if (is_game) QueryPerformanceCounter(&w0);
                }
                ~PltTimer() {
                    if (idx >= kPltCacheMax) return;
                    ULONG64 c1 = 0;
                    if (QueryThreadCycleTime(GetCurrentThread(), &c1) && c1 > c0) {
                        g_plt_cycles[idx].fetch_add(c1 - c0, std::memory_order_relaxed);
                    }
                    if (is_game) {
                        LARGE_INTEGER w1;
                        QueryPerformanceCounter(&w1);
                        if (w1.QuadPart > w0.QuadPart) {
                            g_plt_wall[idx].fetch_add(
                                static_cast<uint64_t>(w1.QuadPart - w0.QuadPart),
                                std::memory_order_relaxed);
                        }
                    }
                }
            } plt_timer(plt_index);

            static const std::string  s_empty_name;
            if (plt_index < kPltCacheMax) {
                g_plt_counts[plt_index].fetch_add(1, std::memory_order_relaxed);
            }
            // Kareyi SUREN thread'i tespit et: flip'i kim cagiriyorsa odur.
            // ExecutionThread degil (olculdu: orada HLE icinde 0 ms geciyor) -
            // C2 dongusu oyunun kendi olusturdugu bir pthread'de calisiyor.
            if (g_plt_rn_cache[plt_index] != nullptr &&
                g_plt_rn_cache[plt_index]->find("Flip") != std::string::npos) {
                g_game_tid = GetCurrentThreadId();
            }
            static std::deque<std::string> s_interned; // UNKNOWN_PLT_* kalici depo
            static std::mutex s_intern_mutex;

            const std::string* fn_ptr = nullptr;
            const std::string* rn_ptr = nullptr;
            if (plt_index < kPltCacheMax) {
                fn_ptr = g_plt_fn_cache[plt_index];
                rn_ptr = g_plt_rn_cache[plt_index];
            }
            if (fn_ptr == nullptr) {
                const std::string* f = nullptr;
                const std::string* r = &s_empty_name;
                auto it = g_plt_names.find(plt_index);
                if (it != g_plt_names.end()) {
                    f = &it->second;
                    auto it2 = g_nid_to_name.find(it->second);
                    if (it2 == g_nid_to_name.end() && it->second.size() >= 11) {
                        // NID son eki (#X#Y) KUTUPHANE/MODUL indeksidir ve oyuna
                        // gore degisir; fonksiyonu tekil belirleyen sey 11
                        // karakterlik NID'in kendisidir. Tam eslesme yoksa onekle
                        // ara. Aksi halde isim cozulmuyor ve fonksiyon default
                        // stub'a (RAX=0) dusuyordu.
                        // Gercek ornek: QOQtbeDqsT4#N#O = sceAudioOutOutput;
                        // tabloda yalnizca #T#T/#S#N/... varyantlari vardi, bu
                        // yuzden ses cikisi stub'lanip audio thread'i bosa
                        // donuyordu (tum PLT cagrilarinin ~%25'i).
                        const auto& pidx = NidPrefixIndex();
                        auto pit = pidx.find(it->second.substr(0, 11));
                        if (pit != pidx.end() && pit->second != nullptr) {
                            r = pit->second; // onekle cozuldu
                            static std::atomic<int> s_pfx{0};
                            if (s_pfx.fetch_add(1, std::memory_order_relaxed) < 24) {
                                printf("[NID-PREFIX] %s -> %s (son ek farkli, onekle cozuldu)\n",
                                       it->second.c_str(), r->c_str());
                                fflush(stdout);
                            }
                        }
                    } else if (it2 != g_nid_to_name.end()) {
                        r = &it2->second;
                    }
                } else {
                    std::lock_guard<std::mutex> lk(s_intern_mutex);
                    s_interned.push_back("UNKNOWN_PLT_" + std::to_string(plt_index));
                    f = &s_interned.back();
                }
                if (plt_index < kPltCacheMax) {
                    g_plt_fn_cache[plt_index] = f;
                    g_plt_rn_cache[plt_index] = r;
                }
                fn_ptr = f;
                rn_ptr = r;
            }
            const std::string& func_name     = *fn_ptr;
            const std::string& readable_name = *rn_ptr;

            // ================================================================
            // YIGIN KORUYUCUSU (PSEMU_STACK_GUARD=1)
            // ================================================================
            // HLE handler'i calisirken MISAFIR KODU DURUYOR. Dolayisiyla o
            // sirada misafir yiginin da degismemesi gerekir - donus adresi
            // dahil. Girişte yiginin bir penceresini kopyalayip cikista
            // karsilastiriyoruz: fark varsa yigini bozan cagriyi ADIYLA
            // yakalamis oluyoruz.
            //
            // Neden gerekli: Astro Bot'ta cokmeler bir komutun ORTASINA
            // dusuyor ve kosudan kosuya degisiyor - klasik "donus adresi
            // ezildi" belirtisi. Kod butunlugu temiz cikti, yani bozulma
            // calisma zamaninda oluyor.
            //
            // Varsayilan KAPALI: cagri basina 2 x 128 bayt kopya, saniyede
            // ~150.000 cagrida olcülebilir maliyet.
            struct StackGuard {
                enum { kWin = 128 };
                static bool Enabled() {
                    static const bool e = [] {
                        const char* v = getenv("PSEMU_STACK_GUARD");
                        return v != nullptr && v[0] != '0';
                    }();
                    return e;
                }
                PCONTEXT           c;
                const std::string* nm;
                uint64_t           rsp0 = 0;
                uint8_t            snap[kWin];
                bool               armed = false;

                StackGuard(PCONTEXT ctx_, const std::string* name) : c(ctx_), nm(name) {
                    if (!Enabled()) return;
                    rsp0 = c->Rsp;
                    if (SafeReadable(reinterpret_cast<void*>(rsp0), kWin)) {
                        memcpy(snap, reinterpret_cast<void*>(rsp0), kWin);
                        armed = true;
                    }
                }
                ~StackGuard() {
                    if (!armed) return;
                    // Handler RET'i simule ettiyse RSP 8 artmis olur; pencereyi
                    // yine ESKI rsp0'dan karsilastiriyoruz.
                    if (!SafeReadable(reinterpret_cast<void*>(rsp0), kWin)) return;
                    const uint8_t* now = reinterpret_cast<const uint8_t*>(rsp0);
                    for (size_t i = 0; i < kWin; i++) {
                        if (now[i] == snap[i]) continue;
                        std::stringstream sg;
                        sg << "[YIGIN-BOZULDU] '" << (nm ? *nm : std::string("?"))
                           << "' cagrisi misafir yiginini degistirdi! rsp=0x" << std::hex << rsp0
                           << " ofset +" << std::dec << i << " : 0x" << std::hex
                           << static_cast<int>(snap[i]) << " -> 0x" << static_cast<int>(now[i])
                           << " | eski donus=0x"
                           << *reinterpret_cast<const uint64_t*>(snap)
                           << " yeni donus=0x" << *reinterpret_cast<const uint64_t*>(now)
                           << std::dec;
                        LOG_ERROR(sg.str());
                        break;
                    }
                }
            } stack_guard(ctx, readable_name.empty() ? fn_ptr : rn_ptr);

            // Halka tampona kaydet: cokme aninda son cagrilar dokulecek.
            {
                const uint32_t slot = g_plt_trace_pos.fetch_add(1, std::memory_order_relaxed) %
                                      kPltTraceMax;
                PltTrace& e = g_plt_trace[slot];
                e.name = readable_name.empty() ? fn_ptr : rn_ptr;
                e.rsp  = ctx->Rsp;
                e.ret  = SafeReadable(reinterpret_cast<void*>(ctx->Rsp), 8)
                             ? *reinterpret_cast<uint64_t*>(ctx->Rsp)
                             : 0;
                e.tid  = GetCurrentThreadId();
            }
            
            // SPIN/ORAN MONITORU: LOG-FILTRE bir fonksiyonu 8 cagridan sonra
            // susturdugu icin sonsuz donguler log'da GORUNMUYORDU (menu donmasi).
            // Toplam PLT cagri sayacini periyodik basip o anki fonksiyonu
            // gosteriyoruz -> hangi fn spin ediyor kesin belli olur.
            {
                static std::atomic<uint64_t> s_plt_total{0};
                s_plt_total.fetch_add(1, std::memory_order_relaxed);
            }

            // ============================================================
            // HIZLI YOL: sicak fonksiyonlar icin string-karsilastirmasiz
            // dispatch. Olculdu: handler basina ~14us CPU; buyuk kismi dev
            // if-else zincirindeki onlarca "readable_name == literal"
            // karsilastirmasi (memset ~60 dal derinde). Asagidaki 6 op
            // cagrilarin ~%75'i. Op kodu plt_index basina BIR KEZ cozulur,
            // sonra switch ile dogrudan islenir; semantik mevcut
            // handler'larla AYNI (kod oradan birebir alindi).
            // ============================================================
            enum : uint8_t { FOP_UNRESOLVED = 0, FOP_NONE, FOP_MEMSET, FOP_MEMCPY,
                             FOP_MTX_LOCK, FOP_MTX_UNLOCK, FOP_ERRNO, FOP_RET0,
                             FOP_MEMCMP, FOP_MEMMOVE };
            static uint8_t s_fop[kPltCacheMax] = {};
            uint8_t fop = (plt_index < kPltCacheMax) ? s_fop[plt_index] : FOP_NONE;
            if (fop == FOP_UNRESOLVED && plt_index < kPltCacheMax) {
                uint8_t f = FOP_NONE;
                if      (readable_name == "memset")                 f = FOP_MEMSET;
                else if (readable_name == "memcpy")                 f = FOP_MEMCPY;
                else if (readable_name == "scePthreadMutexLock" ||
                         readable_name == "pthread_mutex_lock")     f = FOP_MTX_LOCK;
                else if (readable_name == "scePthreadMutexUnlock" ||
                         readable_name == "pthread_mutex_unlock")   f = FOP_MTX_UNLOCK;
                else if (readable_name == "memcmp" || readable_name == "bcmp") f = FOP_MEMCMP;
                else if (readable_name == "memmove")                f = FOP_MEMMOVE;
                else if (readable_name == "__error")                f = FOP_ERRNO;
                // Sik cagrilan ve davranisi "hicbir sey yap, 0 don" olan isimli
                // fonksiyonlar: nids.h'e isim eklendiginde hizli yolu kaybetmesinler.
                // (_Locksyslock/_Unlocksyslock MSVC STL locale kodunda cok sik cagrilir.)
                else if (readable_name == "_Locksyslock" ||
                         readable_name == "_Unlocksyslock" ||
                         readable_name == "uncaught_exception") f = FOP_RET0;
                // KALDIRILDI: sceKernelWaitSema icin FOP_RET0.
                //
                // Bu hizli yol, WaitSema HENUZ IMPLEMENTE DEGILKEN eklenmisti
                // ("davranisi degistirmez, sadece string karsilastirmasini
                // atlar"). Sonradan GERCEK semafor implementasyonu yazildi
                // (asagidaki "SEMAFORLAR - ZINCIRDEN ONCE" blogu) ama bu satir
                // kaldirilmadi - ve hizli yol o bloktan ONCE calistigi icin
                // gercek implementasyonu GOLGELIYORDU. Yani semafor kodu
                // WaitSema icin olu koddu; cagri her zaman aninda 0 donuyordu.
                //
                // Sonuc olculdu (Astro Bot, 30 saniyelik aralik):
                //     sceKernelWaitSema  9.166.328 cagri  (~305.000/sn)
                //     VirtualQuery      18.332.654
                //     toplam exception  76.071.972
                // Bekleyen is parcaciklari bloke olmak yerine spin ediyor,
                // ~9 cekirdek yaniyor ve ana thread CPU bulamadigi icin
                // video-out kaydi surunuyordu.
                s_fop[plt_index] = f;
                fop = FOP_NONE; // ILK cagri normal yoldan gitsin (loglanabilsin)
            }
            if (fop > FOP_NONE) {
                switch (fop) {
                    case FOP_MEMSET: {
                        void*  dst = reinterpret_cast<void*>(ctx->Rdi);
                        size_t n   = static_cast<size_t>(ctx->Rdx);
                        if (dst != nullptr && n != 0 && SafeWritable(dst, n)) {
                            memset(dst, static_cast<int>(ctx->Rsi), n);
                        }
                        ctx->Rax = ctx->Rdi;
                        break;
                    }
                    case FOP_MEMCPY: {
                        void*  dst = reinterpret_cast<void*>(ctx->Rdi);
                        void*  src = reinterpret_cast<void*>(ctx->Rsi);
                        size_t n   = static_cast<size_t>(ctx->Rdx);
                        if (dst != nullptr && src != nullptr && n != 0 &&
                            SafeReadable(src, n) && SafeWritable(dst, n)) {
                            memcpy(dst, src, n);
                        }
                        ctx->Rax = ctx->Rdi;
                        break;
                    }
                    case FOP_MTX_LOCK: {
                        GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rdi));
                        if (m != nullptr) EnterCriticalSection(&m->cs);
                        ctx->Rax = 0;
                        break;
                    }
                    case FOP_MTX_UNLOCK: {
                        GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rdi));
                        if (m != nullptr) LeaveCriticalSection(&m->cs);
                        ctx->Rax = 0;
                        break;
                    }
                    case FOP_MEMCMP: {
                        const void* a = reinterpret_cast<const void*>(ctx->Rdi);
                        const void* b = reinterpret_cast<const void*>(ctx->Rsi);
                        size_t n = static_cast<size_t>(ctx->Rdx);
                        int r = 0;
                        if (n && SafeReadable(a, n) && SafeReadable(b, n)) r = memcmp(a, b, n);
                        ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                        break;
                    }
                    case FOP_MEMMOVE: {
                        void*       dst = reinterpret_cast<void*>(ctx->Rdi);
                        const void* src = reinterpret_cast<const void*>(ctx->Rsi);
                        size_t      n   = static_cast<size_t>(ctx->Rdx);
                        if (dst != nullptr && src != nullptr && n != 0 &&
                            SafeReadable(src, n) && SafeWritable(dst, n)) {
                            memmove(dst, src, n);
                        }
                        ctx->Rax = ctx->Rdi;
                        break;
                    }
                    case FOP_ERRNO: ctx->Rax = reinterpret_cast<uint64_t>(&g_guest_errno); break;
                    case FOP_RET0:  ctx->Rax = 0; break;
                    default: break;
                }
                // RET simulasyonu (normal yoldakinin aynisi). Donus adresi
                // stack'in tepesinde; PLT cagrisinda oyun stack'i her zaman
                // gecerli oldugu icin SafeReadable'in VirtualQuery maliyetini
                // odemeden dogrudan okuyoruz.
                ctx->Rip = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Gurultu filtresi: her fonksiyonu ilk N kez logla (asagidaki
            // RET-simulasyon logu da ayni karara bagli).
            bool log_this = ShouldLogPlt(plt_index,
                                         readable_name.empty() ? func_name : readable_name);

            // THREAD-ORNEK icin: bu thread SU AN hangi HLE'nin icinde?
            // Ornekleyici baska thread'lerin TLS'ini okuyamadigi icin kucuk
            // bir global tablo tutuyoruz. Log filtresinden BAGIMSIZ - takilan
            // thread'i bulmak icin tam da susturulmus cagrilar gerekiyor.
            RecordThreadHle(plt_index);

            if (log_this) {
                std::stringstream hle_ss;
                hle_ss << "[PLT-HLE] " << (readable_name.empty() ? func_name : readable_name)
                       << " [PLT#" << plt_index << "]";
                if (readable_name.empty()) {
                    hle_ss << " (NID: " << func_name << ") RDI=0x" << std::hex << ctx->Rdi
                           << " RSI=0x" << ctx->Rsi << " RDX=0x" << ctx->Rdx << std::dec;
                }
                LOG_INFO(hle_ss.str());
            }

            // ========================================================
            // strtok / strtok_r - ZINCIRDEN ONCE
            // ========================================================
            // char* dondurur ve DURUM TUTAR. Stub'landiginda NULL donuyordu;
            // bir ayristirma dongusu NULL alinca "basardim" bayragini set edip
            // BOS bir yapi birakiyor. Astro Bot'taki cokme izi tam buydu:
            // "global ilklendi bayragi set, ama isaret ettigi veri yok".
            // Girdi dizesini YERINDE degistirir (ayirici yerine NUL yazar).
            if (readable_name == "strtok" || readable_name == "strtok_r") {
                static thread_local char* t_save = nullptr;
                const bool is_r = (readable_name == "strtok_r");

                char*       s     = reinterpret_cast<char*>(ctx->Rdi);
                const char* delim = reinterpret_cast<const char*>(ctx->Rsi);
                char**      savep = is_r ? reinterpret_cast<char**>(ctx->Rdx) : nullptr;

                // Ayirici kumesi (en fazla 256 farkli bayt)
                bool isdelim[256] = {};
                if (delim != nullptr) {
                    const std::string d = SafeReadCString(delim, 256);
                    for (unsigned char c : d) isdelim[c] = true;
                }

                char* cur = s;
                if (cur == nullptr) {
                    cur = is_r ? (savep && SafeReadable(savep, 8) ? *savep : nullptr) : t_save;
                }

                char* result = nullptr;
                if (cur != nullptr) {
                    // Bastaki ayiricilari atla
                    while (SafeReadable(cur, 1) && *cur != '\0' &&
                           isdelim[static_cast<unsigned char>(*cur)]) {
                        cur++;
                    }
                    if (SafeReadable(cur, 1) && *cur != '\0') {
                        result   = cur;
                        // Parcanin sonunu bul
                        while (SafeReadable(cur, 1) && *cur != '\0' &&
                               !isdelim[static_cast<unsigned char>(*cur)]) {
                            cur++;
                        }
                        if (SafeReadable(cur, 1) && *cur != '\0') {
                            if (SafeWritable(cur, 1)) *cur = '\0';
                            cur++;
                        }
                    } else {
                        cur = nullptr;
                    }
                }

                if (is_r) {
                    if (savep != nullptr && SafeWritable(savep, 8)) *savep = cur;
                } else {
                    t_save = cur;
                }

                static std::atomic<int> s_tk{0};
                if (s_tk.fetch_add(1) < 8) {
                    printf("[STRTOK] -> %s\n",
                           result ? SafeReadCString(result, 64).c_str() : "(NULL)");
                    fflush(stdout);
                }

                ctx->Rax  = reinterpret_cast<uint64_t>(result);
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // Kucuk ama SIFIR DONMEMESI GEREKEN cagrilar
            // ========================================================
            if (readable_name == "scePthreadGetthreadid" ||
                readable_name == "pthread_getthreadid_np") {
                // Thread kimligi indeks/anahtar olarak kullanilabiliyor; 0
                // donmek cakismalara yol acar.
                ctx->Rax  = GetCurrentThreadId();
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "_ZSt14_Random_devicev") {
                // std::random_device(): 0 donmek tohumlamayi bozar.
                static std::atomic<uint32_t> s_rnd{0x9E3779B9u};
                uint32_t x = s_rnd.fetch_add(0x6D2B79F5u, std::memory_order_relaxed);
                x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
                ctx->Rax  = x;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // puts / fputs - OYUNUN MESAJLARI - ZINCIRDEN ONCE
            // ========================================================
            // Astro Bot cokmeden hemen once puts cagiriyordu, yani bize bir sey
            // SOYLUYOR. Stub'landigi icin mesaj kayboluyordu. Artik [GAME-LOG]
            // olarak yaziyoruz - hata ayiklamada en degerli tek kaynak budur.
            if (readable_name == "puts" || readable_name == "fputs") {
                // puts(s): RDI=s | fputs(s, stream): RDI=s, RSI=stream
                const std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi));
                if (!s.empty()) LOG_INFO("[GAME-LOG] " + s);
                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // sceSystemServiceParamGetInt - ZINCIRDEN ONCE
            // ========================================================
            // (param_id, int* out) -> sistem ayarini dondurur. Stub 0 donuyor
            // AMA *out'a HICBIR SEY YAZMIYOR; oyun ilklenmemis bir degeri
            // ayar sanip onunla dizi indeksliyor (olculdu: RDI=1 ile
            // "mov rdx,[rdi+rcx*8]" -> adres 0x1'e erisim).
            // Makul varsayilanlar yaziyoruz.
            if (readable_name == "sceSystemServiceParamGetInt") {
                const int32_t id  = static_cast<int32_t>(ctx->Rdi);
                int32_t*      out = reinterpret_cast<int32_t*>(ctx->Rsi);
                int32_t       val = 0;
                switch (id) {
                    case 1:  val = 1; break; // LANG: 1 = English (US)
                    case 2:  val = 0; break; // DATE_FORMAT: YYYY/MM/DD
                    case 3:  val = 0; break; // TIME_FORMAT: 24 saat
                    case 4:  val = 0; break; // TIME_ZONE
                    case 5:  val = 0; break; // SUMMERTIME
                    case 7:  val = 0; break; // GAME_PARENTAL_LEVEL: kisitlama yok
                    case 1000000: val = 0; break; // ENTER_BUTTON_ASSIGN: cross
                    default: val = 0; break;
                }
                if (out != nullptr && SafeWritable(out, 4)) {
                    *out = val;
                }
                static std::atomic<int> s_sp{0};
                if (s_sp.fetch_add(1) < 12) {
                    printf("[SYSPARAM] GetInt(id=%d) -> %d\n", id, val);
                    fflush(stdout);
                }
                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // sceUserService AILESI - ZINCIRDEN ONCE
            // ========================================================
            // Astro Bot'ta iki thread sceUserServiceGetEvent uzerinde SONSUZ
            // donuyordu (thread ornekleyicisiyle olculdu). Sebep: hicbir
            // karsiligi yoktu, genel stub 0 (=SCE_OK, "olay var") donuyordu
            // ama olay yapisina HICBIR SEY yazmiyordu. Oyun her turda bos bir
            // olay isleyip bekledigi LOGIN'i hic goremiyordu.
            //
            // Dogru davranis: bir kez LOGIN olayi ver, sonra NO_EVENT don.
            if (readable_name.rfind("sceUserService", 0) == 0) {
                const int32_t kUserId    = 1;          // tek yerel kullanici
                const uint32_t kNoEvent  = 0x80960009; // SCE_USER_SERVICE_ERROR_NO_EVENT
                uint64_t rax = 0;

                if (readable_name == "sceUserServiceGetEvent") {
                    // SceUserServiceEvent { int32 eventType; int32 userId; }
                    // eventType: 0 = LOGIN, 1 = LOGOUT
                    static std::atomic<int> s_login_sent{0};
                    int32_t* ev = reinterpret_cast<int32_t*>(ctx->Rdi);
                    if (ev != nullptr && SafeWritable(ev, 8) &&
                        s_login_sent.fetch_add(1) == 0) {
                        ev[0] = 0;        // LOGIN
                        ev[1] = kUserId;
                        rax = 0;
                        printf("[USERSERVICE] LOGIN olayi verildi (userId=%d)\n", kUserId);
                        fflush(stdout);
                    } else {
                        rax = kNoEvent;   // baska olay yok
                    }
                } else if (readable_name == "sceUserServiceGetInitialUser") {
                    int32_t* out = reinterpret_cast<int32_t*>(ctx->Rdi);
                    if (out != nullptr && SafeWritable(out, 4)) *out = kUserId;
                } else if (readable_name == "sceUserServiceGetLoginUserIdList") {
                    // SceUserServiceLoginUserIdList { int32 userId[4]; }
                    int32_t* list = reinterpret_cast<int32_t*>(ctx->Rdi);
                    if (list != nullptr && SafeWritable(list, 16)) {
                        list[0] = kUserId;
                        list[1] = list[2] = list[3] = -1; // SCE_USER_SERVICE_USER_ID_INVALID
                    }
                } else if (readable_name == "sceUserServiceGetUserName") {
                    // (userId, char* buf, size_t size)
                    char*  buf = reinterpret_cast<char*>(ctx->Rsi);
                    size_t sz  = static_cast<size_t>(ctx->Rdx);
                    if (buf != nullptr && sz > 0 && SafeWritable(buf, sz)) {
                        snprintf(buf, sz, "psemu");
                    }
                }
                // Geri kalan sceUserService* cagrilari (Initialize, Terminate,
                // GetUserColor, ...) icin 0 = basarili yeterli.

                static std::atomic<int> s_us{0};
                if (s_us.fetch_add(1) < 16) {
                    printf("[USERSERVICE] %s -> 0x%llx\n", readable_name.c_str(),
                           (unsigned long long)rax);
                    fflush(stdout);
                }
                ctx->Rax  = rax;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // pthread THREAD-SPECIFIC ANAHTARLARI - ZINCIRDEN ONCE
            // ========================================================
            // Hicbiri implemente degildi ve hepsi RAX=0 donuyordu. Bu SESSIZ
            // ama agir bir hata:
            //   key_create(key*, dtor) 0 (basarili) donuyor ama *key'e HICBIR
            //     SEY YAZMIYOR -> oyun cop bir anahtar degeri kullaniyor
            //   setspecific(key, val)  hicbir sey yapmiyor -> deger kayboluyor
            //   getspecific(key)       daima 0 -> oyun "ilklenmemis" saniyor
            // Astro Bot'ta cokmeden hemen once pthread_getspecific cagriliyor
            // ve ardindan imaj disi bir isaretciye dallaniyor.
            //
            // Windows TLS slotlariyla dogrudan esliyoruz: anahtar = TLS indeksi.
            if (readable_name == "pthread_key_create" ||
                readable_name == "scePthreadKeyCreate") {
                // (key*, destructor) -> *key = yeni indeks, 0 don
                uint32_t*  key = reinterpret_cast<uint32_t*>(ctx->Rdi);
                const DWORD idx = TlsAlloc();
                int rc = 0;
                if (idx == TLS_OUT_OF_INDEXES) {
                    rc = 11; // EAGAIN
                } else if (key != nullptr && SafeWritable(key, 4)) {
                    *key = static_cast<uint32_t>(idx);
                } else {
                    TlsFree(idx);
                    rc = 22; // EINVAL
                }
                // NOT: yikici (destructor) cagrilmiyor. Oyun thread bitiminde
                // temizlik bekliyorsa sizinti olur; ama yanlis deger dondurmekten
                // cok daha iyisi.
                static std::atomic<int> s_kc{0};
                if (s_kc.fetch_add(1) < 8) {
                    printf("[PTHREAD-TLS] key_create -> anahtar=%lu rc=%d\n", idx, rc);
                    fflush(stdout);
                }
                ctx->Rax  = static_cast<uint64_t>(static_cast<int64_t>(rc));
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "pthread_setspecific" ||
                readable_name == "scePthreadSetspecific") {
                const DWORD idx = static_cast<DWORD>(ctx->Rdi);
                TlsSetValue(idx, reinterpret_cast<void*>(ctx->Rsi));
                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "pthread_getspecific" ||
                readable_name == "scePthreadGetspecific") {
                const DWORD idx = static_cast<DWORD>(ctx->Rdi);
                ctx->Rax  = reinterpret_cast<uint64_t>(TlsGetValue(idx));
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "pthread_key_delete" ||
                readable_name == "scePthreadKeyDelete") {
                TlsFree(static_cast<DWORD>(ctx->Rdi));
                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // sceLibcMspaceCreate ile verilen kapasiteyi tutamaca bagli
            // sakliyoruz; MallocStats bunu bildirmek zorunda.
            // ========================================================
            // sceLibcMspace* (Sony mspace ayiricisi) - ZINCIRDEN ONCE
            // ========================================================
            // Astro Bot'un ic ayiricisi bunun uzerine kurulu. Implemente
            // olmadigi icin sceLibcMspaceMalloc 0 donduruyordu ve oyun donen
            // NULL'a nesne yazmaya calisip cokuyordu (WRITE @ 0x0, RVA
            // 0x70a932c: "mov [rax], rax"). NID hicbir veritabaninda yoktu;
            // modul tablosundan libc oldugu anlasilip tuzlu SHA1 ile cozuldu
            // (OJjm-QOIHlI, bkz. tools/scripts/nid_libc.py).
            //
            // mspace TUTAMACINI YOK SAYIYORUZ: gercek bir dlmalloc havuzu
            // kurmak yerine tum tahsisleri malloc/_Znwm ile AYNI ayiriciya
            // yonlendiriyoruz. Oyun ayni isaretciyi free/delete/MspaceFree ile
            // birakabildigi icin tek ayirici kullanmak SART.
            // C++ global serbest birakma operatorleri. NID'leri toplu cozumden
            // geldi (bkz. tools/scripts/nid_bulk.py). new/malloc/MspaceMalloc
            // ile AYNI ayiriciyi kullandigimiz icin hepsi _aligned_free.
            // Hizalanmis ve nothrow varyantlarinda da ilk arguman isaretcidir.
            if (readable_name == "_ZdlPv" || readable_name == "_ZdaPv" ||
                readable_name == "_ZdlPvm" || readable_name == "_ZdaPvm" ||
                readable_name == "_ZdlPvSt11align_val_t" ||
                readable_name == "_ZdaPvSt11align_val_t" ||
                readable_name == "_ZdlPvmSt11align_val_t" ||
                readable_name == "_ZdaPvmSt11align_val_t" ||
                readable_name == "_ZdlPvRKSt9nothrow_t" ||
                readable_name == "_ZdaPvRKSt9nothrow_t") {
                void* p = reinterpret_cast<void*>(ctx->Rdi);
                if (p != nullptr) _aligned_free(p);
                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "sceLibcMspaceMalloc" ||
                readable_name == "sceLibcMspaceCalloc" ||
                readable_name == "sceLibcMspaceMemalign" ||
                readable_name == "sceLibcMspaceRealloc" ||
                readable_name == "sceLibcMspaceFree" ||
                readable_name == "sceLibcMspaceCreate" ||
                readable_name == "sceLibcMspaceDestroy" ||
                readable_name == "sceLibcMspaceMallocStats" ||
                readable_name == "sceLibcMspaceMallocStatsFast" ||
                readable_name == "sceLibcMspaceIsHeapEmpty" ||
                readable_name == "sceLibcMspaceMallocUsableSize") {
                uint64_t rv = 0;
                if (readable_name == "sceLibcMspaceCreate") {
                    // Create(name, base, capacity, flag) -> mspace tutamaci.
                    // Sifir olmayan sahte bir tutamac yeter; gercek havuzu
                    // kullanmiyoruz. Oyun bunu sadece geri veriyor.
                    // KAPASITEYI SAKLIYORUZ: MallocStats bunu bildirmek
                    // zorunda (asagiya bkz).
                    static uint64_t s_fake_mspace = 0x4D53504100000001ull; // "MSPA..."
                    rv = ++s_fake_mspace;
                    // Create(name, base, capacity, flag): RSI=base, RDX=capacity
                    MspaceRemember(rv, static_cast<uint64_t>(ctx->Rsi),
                                       static_cast<uint64_t>(ctx->Rdx));
                    printf("[MSPACE] Create -> handle=0x%llx base=0x%llx cap=0x%llx\n",
                           (unsigned long long)rv, (unsigned long long)ctx->Rsi,
                           (unsigned long long)ctx->Rdx);
                    fflush(stdout);
                } else if (readable_name == "sceLibcMspaceMallocStats" ||
                           readable_name == "sceLibcMspaceMallocStatsFast") {
                    // (SceLibcMspace msp, SceLibcMallocManagedSize* out)
                    //
                    // Eskiden yapiya HIC DOKUNMADAN 0 donuyorduk. Astro Bot
                    // GPU ayiricisi tahsisten ONCE bu istatistige bakip
                    // "bos yer = system - inuse" hesapliyor; sifir yapiyla bu
                    // 0 cikiyor ve hicbir sey tahsis edilmeden reddediliyordu:
                    //     Out of graphics memory [Onion].
                    //     size = 2097152, used 0/192937984, count 1
                    // ("used 0" tam da doldurulmamis inuse alanidir.)
                    //
                    // SceLibcMallocManagedSize (Sony libc):
                    //   uint16 size; uint16 version; uint32 reserved;
                    //   size_t maxSystemSize, currentSystemSize,
                    //          maxInuseSize,  currentInuseSize;
                    uint8_t* st = reinterpret_cast<uint8_t*>(ctx->Rsi);
                    if (st != nullptr && SafeWritable(st, 40)) {
                        const uint64_t cap = MspaceCapacity(ctx->Rdi);
                        *reinterpret_cast<uint16_t*>(st + 0) = 40; // size
                        *reinterpret_cast<uint16_t*>(st + 2) = 0;  // version
                        *reinterpret_cast<uint32_t*>(st + 4) = 0;  // reserved
                        *reinterpret_cast<uint64_t*>(st + 8)  = cap; // maxSystemSize
                        *reinterpret_cast<uint64_t*>(st + 16) = cap; // currentSystemSize
                        const uint64_t used = MspaceUsed(ctx->Rdi);
                        *reinterpret_cast<uint64_t*>(st + 24) = used; // maxInuseSize
                        *reinterpret_cast<uint64_t*>(st + 32) = used; // currentInuseSize
                    }
                    rv = 0;
                } else if (readable_name == "sceLibcMspaceDestroy") {
                    rv = 0;
                } else if (readable_name == "sceLibcMspaceIsHeapEmpty") {
                    rv = 0; // "bos degil"
                } else if (readable_name == "sceLibcMspaceFree") {
                    // Free(mspace, ptr)
                    void* p = reinterpret_cast<void*>(ctx->Rsi);
                    if (p != nullptr) _aligned_free(p);
                    rv = 0;
                } else if (readable_name == "sceLibcMspaceMallocUsableSize") {
                    rv = LookupAllocSize(reinterpret_cast<void*>(ctx->Rsi));
                } else {
                    // Malloc(mspace, size) / Calloc(mspace, n, size) /
                    // Memalign(mspace, align, size) / Realloc(mspace, ptr, size)
                    size_t n = 0;
                    size_t align = 16;
                    void*  old_p = nullptr;
                    if (readable_name == "sceLibcMspaceCalloc") {
                        n = static_cast<size_t>(ctx->Rsi) * static_cast<size_t>(ctx->Rdx);
                    } else if (readable_name == "sceLibcMspaceMemalign") {
                        align = static_cast<size_t>(ctx->Rsi);
                        if (align < 16 || (align & (align - 1)) != 0) align = 16;
                        n = static_cast<size_t>(ctx->Rdx);
                    } else if (readable_name == "sceLibcMspaceRealloc") {
                        old_p = reinterpret_cast<void*>(ctx->Rsi);
                        n     = static_cast<size_t>(ctx->Rdx);
                    } else {
                        n = static_cast<size_t>(ctx->Rsi);
                    }
                    // Tutamac GERCEK bir bolge uzerine kurulduysa (GPU Onion
                    // havuzu gibi) tahsisi ORADAN yap: oyun bu isaretcileri
                    // GPU'ya veriyor ve havuz araliginda olmalarini bekliyor.
                    // Realloc'ta eski veriyi tasimak gerektigi icin bump
                    // yolunu yalnizca yeni tahsislerde kullaniyoruz.
                    if (old_p == nullptr) {
                        if (void* gp = MspaceBumpAlloc(ctx->Rdi, n ? n : 16, align)) {
                            memset(gp, 0, n ? n : 16);
                            RegisterAllocSize(gp, n);
                            rv = reinterpret_cast<uint64_t>(gp);
                            goto mspace_done;
                        }
                    }
                    // Bolgesiz tutamac: host yigini. malloc yolundaki ayni
                    // comert tampon (oyun bazen istediginden fazlasina dokunuyor).
                    {
                    const size_t alloc_sz = n ? (n + 65536) : 65536;
                    void*        p        = _aligned_malloc(alloc_sz, align);
                    if (p != nullptr) {
                        memset(p, 0, alloc_sz);
                        if (old_p != nullptr) {
                            const size_t old_n = LookupAllocSize(old_p);
                            const size_t cp    = (old_n < n) ? old_n : n;
                            if (cp != 0) memcpy(p, old_p, cp);
                            _aligned_free(old_p);
                        }
                        RegisterAllocSize(p, n);
                    }
                    rv = reinterpret_cast<uint64_t>(p);
                    }
                }
            mspace_done:
                static std::atomic<uint64_t> s_ms{0};
                const uint64_t mn = s_ms.fetch_add(1) + 1;
                if (mn <= 6 || (mn % 5000ull) == 0) {
                    printf("[MSPACE] #%llu %s(mspace=0x%llx rsi=0x%llx rdx=0x%llx) -> 0x%llx\n",
                           static_cast<unsigned long long>(mn), readable_name.c_str(),
                           static_cast<unsigned long long>(ctx->Rdi),
                           static_cast<unsigned long long>(ctx->Rsi),
                           static_cast<unsigned long long>(ctx->Rdx),
                           static_cast<unsigned long long>(rv));
                    fflush(stdout);
                }
                ctx->Rax  = rv;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // COMMON DIALOG (mesaj penceresi) - ZINCIRDEN ONCE
            // ========================================================
            // Astro Bot acilista bir sistem mesaj penceresi acip durumunu
            // 60 Hz'de yokluyor (olculdu: sceMsgDialogUpdateStatus +
            // sceKernelUsleep(16000) dongusu). Varsayilan stub 0 = NONE
            // donduruyordu, yani pencere ASLA bitmiyor ve oyun o dongude
            // sonsuza kadar kaliyordu.
            //
            // Sony durum degerleri: NONE=0, INITIALIZED=1, RUNNING=2, FINISHED=3.
            // Pencereyi "hemen bitti" olarak bildiriyoruz ki oyun devam etsin.
            if (readable_name == "sceMsgDialogUpdateStatus" ||
                readable_name == "sceMsgDialogGetStatus" ||
                readable_name == "sceCommonDialogUpdateStatus" ||
                readable_name == "sceErrorDialogUpdateStatus" ||
                readable_name == "sceErrorDialogGetStatus" ||
                readable_name == "sceSaveDataDialogUpdateStatus" ||
                readable_name == "sceSaveDataDialogGetStatus" ||
                readable_name == "sceNpProfileDialogUpdateStatus" ||
                readable_name == "sceImeDialogGetStatus") {
                ctx->Rax  = 3; // SCE_COMMON_DIALOG_STATUS_FINISHED
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "sceMsgDialogGetResult" ||
                readable_name == "sceErrorDialogGetResult" ||
                readable_name == "sceSaveDataDialogGetResult") {
                // Sonuc yapisi: {int32 mode; int32 result; int32 buttonId; ...}
                //
                // DIKKAT - BURADA BIR HATA YAPMISTIM: "yapinin tam boyutunu
                // bilmiyoruz, 64 bayt guvenli bir ust sinir" deyip 64 bayt
                // memset ediyordum. SceMsgDialogResult 44 bayttir
                // (mode 4 + result 4 + buttonId 4 + reserved[32]); yapi YIGINDA
                // duruyorsa 20 bayt tasip CAGIRANIN DONUS ADRESINI eziyordum.
                // Belirtisi tam da gordugumuz seydi: komutun ORTASINA dusen,
                // kosular arasi DEGISEN "gecersiz komut" cokmeleri.
                //
                // Kural: boyutunu KANITLAYAMADIGIMIZ bir yapiya asla tahminle
                // yazma. Yalnizca anlamini bildigimiz ilk 12 bayti yaziyoruz.
                int32_t* res = reinterpret_cast<int32_t*>(ctx->Rdi);
                if (res != nullptr && SafeWritable(res, 12)) {
                    res[0] = 0; // mode
                    res[1] = 0; // result = OK
                    res[2] = 1; // buttonId = OK
                }
                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // SEMAFORLAR - ZINCIRDEN ONCE
            // ========================================================
            // Onceden HICBIRI implemente degildi: WaitSema aninda 0 (basarili)
            // donuyordu. Oyunun "sayac gelene kadar bekle" istegi boylece aninda
            // donen bir yoklamaya donusuyor ve donguye giriyordu - olculdu:
            // saniyede ~42.000 WaitSema cagrisi, 1 tane CreateSema, SignalSema
            // logda HIC yok.
            //
            // ABI (SysV): CreateSema(out RDI, name RSI, attr RDX, init RCX, max R8, opt R9)
            //             WaitSema(sema RDI, need RSI, timeout_us* RDX)
            //             SignalSema(sema RDI, count RSI)
            //             PollSema(sema RDI, need RSI)
            if (readable_name == "sceKernelCreateSema" || readable_name == "sceKernelWaitSema" ||
                readable_name == "sceKernelSignalSema" || readable_name == "sceKernelPollSema" ||
                readable_name == "sceKernelDeleteSema" ||
                readable_name == "sceKernelCancelSema") {
                int rc = 0;
                if (readable_name == "sceKernelCreateSema") {
                    uint64_t* out  = reinterpret_cast<uint64_t*>(ctx->Rdi);
                    const int init = static_cast<int>(ctx->Rcx);
                    int       maxv = static_cast<int>(ctx->R8);
                    if (maxv <= 0) maxv = 0x7FFFFFFF;
                    HANDLE h = CreateSemaphoreW(nullptr, init, maxv, nullptr);
                    if (out != nullptr && SafeWritable(out, 8) && h != nullptr) {
                        *out = reinterpret_cast<uint64_t>(h);
                    } else {
                        rc = -2147352575; // SCE_KERNEL_ERROR_ENOMEM benzeri
                    }
                    printf("[SEMA] create -> handle=%p init=%d max=%d\n", h, init, maxv);
                    fflush(stdout);
                } else if (readable_name == "sceKernelSignalSema") {
                    HANDLE    h = reinterpret_cast<HANDLE>(ctx->Rdi);
                    const int n = static_cast<int>(ctx->Rsi);
                    if (h != nullptr) ReleaseSemaphore(h, n > 0 ? n : 1, nullptr);
                    static std::atomic<uint64_t> s_sig{0};
                    const uint64_t sg = s_sig.fetch_add(1) + 1;
                    if (sg <= 4 || (sg % 1000ull) == 0) {
                        printf("[SEMA] signal #%llu handle=%p count=%d\n",
                               static_cast<unsigned long long>(sg), h, n);
                        fflush(stdout);
                    }
                } else if (readable_name == "sceKernelPollSema") {
                    HANDLE h = reinterpret_cast<HANDLE>(ctx->Rdi);
                    rc = (h != nullptr && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) ? 0 : -2147352573;
                } else if (readable_name == "sceKernelWaitSema") {
                    HANDLE          h  = reinterpret_cast<HANDLE>(ctx->Rdi);
                    const uint32_t* tp = reinterpret_cast<const uint32_t*>(ctx->Rdx);
                    // GUVENLIK SINIRI: oyun bu semaforu hic sinyallemiyorsa
                    // INFINITE beklemek kilitlenme demek. En fazla 50 ms bekleyip
                    // basarili donuyoruz - eski davranisin (aninda basari)
                    // korunmus ama saniyede 42.000 yerine en fazla 20 kez
                    // calisan hali. Sinira kac kez carptigimizi sayiyoruz:
                    // sifira yakinsa sinyalleme calisiyor demektir.
                    DWORD ms = 50;
                    bool  had_timeout_arg = false;
                    if (tp != nullptr && SafeReadable(tp, 4)) {
                        had_timeout_arg = true;
                        const uint32_t us = *tp;
                        ms = static_cast<DWORD>(us / 1000u);
                        if (ms > 50) ms = 50;
                    }
                    const DWORD wr = (h != nullptr) ? WaitForSingleObject(h, ms) : WAIT_TIMEOUT;

                    static std::atomic<uint64_t> s_w{0}, s_to{0};
                    const uint64_t wn = s_w.fetch_add(1) + 1;
                    if (wr != WAIT_OBJECT_0) s_to.fetch_add(1, std::memory_order_relaxed);
                    if (wn <= 6 || (wn % 500ull) == 0) {
                        printf("[SEMA] wait #%llu handle=%p need=%d timeout_arg=%d ms=%lu "
                               "sonuc=%s (zaman asimi orani %llu/%llu)\n",
                               static_cast<unsigned long long>(wn), h,
                               static_cast<int>(ctx->Rsi), had_timeout_arg ? 1 : 0, ms,
                               (wr == WAIT_OBJECT_0) ? "SINYAL" : "zaman-asimi",
                               static_cast<unsigned long long>(s_to.load()),
                               static_cast<unsigned long long>(wn));
                        fflush(stdout);
                    }
                    rc = 0; // zaman asiminda da basarili: eski davranis korunuyor
                }
                // DeleteSema/CancelSema: tutamaci kapatmiyoruz (baska thread
                // hala bekliyor olabilir) - scePthreadMutexDestroy ile ayni yaklasim.

                ctx->Rax  = static_cast<uint64_t>(static_cast<int64_t>(rc));
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // sceKernelUsleep - ZINCIRDEN ONCE (hassas uyku)
            // ========================================================
            // Kare suresi 64-67 ms ve tam 4 vblank katina kilitli; VEH ise
            // yalnizca ~7 ms/kare. Geri kalan sure UYKUDA gecmis olmali:
            // bu fonksiyon saniyede ~660 kez cagriliyor (kare basina ~44).
            // Eski hal Sleep(us/1000, en az 1) idi. Windows'ta varsayilan
            // zamanlayici cozunurlugu 15.6 ms'dir, yani Sleep(1) GERCEKTE
            // ~15 ms uyur. Kare basina birkac tanesi bile butun kareyi yer.
            //
            // Cozum: yuksek cozunurluklu bekleme zamanlayicisi (100 ns
            // birimli, thread'e ozel). CPU yakmaz, Sleep'in granulerligine
            // takilmaz. Desteklenmiyorsa Sleep'e duseriz.
            if (readable_name == "sceKernelUsleep" || readable_name == "usleep") {
                const uint64_t us = ctx->Rdi;

                static std::atomic<uint64_t> s_us_calls{0};
                static std::atomic<uint64_t> s_us_total{0};
                s_us_calls.fetch_add(1, std::memory_order_relaxed);
                s_us_total.fetch_add(us, std::memory_order_relaxed);
                const uint64_t uc = s_us_calls.load(std::memory_order_relaxed);
                if (uc <= 8 || (uc % 3000ull) == 0) {
                    printf("[USLEEP] #%llu istenen=%llu us (ortalama %llu us)\n",
                           static_cast<unsigned long long>(uc),
                           static_cast<unsigned long long>(us),
                           static_cast<unsigned long long>(
                               s_us_total.load(std::memory_order_relaxed) / uc));
                    fflush(stdout);
                }

                // Hassas (yuksek cozunurluklu) uyku DENENDI, FPS'i DUSURDU -
                // bkz. yukaridaki not. Eski davranista kaliyoruz.
                if (us == 0) {
                    SwitchToThread();
                } else {
                    const DWORD ms = static_cast<DWORD>(us / 1000);
                    Sleep(ms != 0 ? ms : 1);
                }

                ctx->Rax  = 0;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // __dynamic_cast - ZINCIRDEN ONCE
            // ========================================================
            // KARARSIZLIGIN KAYNAGI. Bu fonksiyonun HIC karsiligi yoktu; zincirin
            // sonundaki varsayilan stub'a dusup RAX=0 donuyordu, yani oyundaki
            // HER dynamic_cast nullptr aliyordu. Olcum: 6 kosunun 5'i T+13-17
            // arasinda "READ violation @ 0x0" ile coktu ve ucunde de cokmeden
            // hemen onceki baskin cagri __dynamic_cast'ti. (Kalan biri ayni
            // yerde takildi.)
            //
            // Itanium C++ ABI:
            //   void* __dynamic_cast(const void* sub, const __class_type_info* src,
            //                        const __class_type_info* dst, ptrdiff_t src2dst)
            // SysV: RDI=sub, RSI=src, RDX=dst, RCX=src2dst
            //
            // Nesnenin GERCEK tipi vtable'dan okunur:
            //   vptr          = *(void**)sub
            //   offset_to_top = *(int64*)(vptr - 16)
            //   typeinfo      = *(void**)(vptr - 8)
            // Tam nesne = sub + offset_to_top.
            //
            // src2dst >= 0 ise "dst, src'nin tekil public non-virtual tabani"
            // demektir ve sonuc sub - src2dst olur (libc++abi'nin hizli yolu).
            // Tam hiyerarsi yurumesi (coklu/sanal kalitim) YAPILMIYOR; o
            // durumlarda 0 donuyoruz - yani eski davranis, daha kotusu degil.
            if (readable_name == "__dynamic_cast") {
                const uint64_t  sub     = ctx->Rdi;
                const uint64_t  dst_ti  = ctx->Rdx;
                const int64_t   s2d     = static_cast<int64_t>(ctx->Rcx);
                uint64_t        result  = 0;
                const char*     karar   = "null";

                if (sub != 0 && SafeReadable(reinterpret_cast<void*>(sub), 8)) {
                    const uint64_t vptr = *reinterpret_cast<uint64_t*>(sub);
                    if (vptr >= 16 && SafeReadable(reinterpret_cast<void*>(vptr - 16), 16)) {
                        const int64_t  off_to_top = *reinterpret_cast<int64_t*>(vptr - 16);
                        const uint64_t obj_ti     = *reinterpret_cast<uint64_t*>(vptr - 8);
                        if (obj_ti == dst_ti) {
                            result = static_cast<uint64_t>(static_cast<int64_t>(sub) + off_to_top);
                            karar  = "tam-tip";
                        } else if (s2d >= 0) {
                            // src2dst >= 0 "cast BASARILIYSA ofset budur" demek;
                            // "basarilidir" DEMEK DEGIL. Ilk surumde bunu ipucuna
                            // bakip dogrudan dondurmustum: nesnenin gercek tipi
                            // hedefle ilgisiz oldugunda oyuna GECERSIZ isaretci
                            // gidiyordu (olculdu: R15 = 0x56415741e5894855, yani
                            // bir fonksiyon prologu bayt dizisi -> kanonik olmayan
                            // adres -> "READ @ 0xffffffffffffffff").
                            // Bu yuzden dst_ti'nin gercekten bir taban olup
                            // olmadigini DOGRULUYORUZ: tek-kalitim zincirini yuru.
                            // __si_class_type_info duzeni: [vptr][name][__base_type]
                            uint64_t ti = obj_ti;
                            bool     ok = false;
                            for (int depth = 0; depth < 16; depth++) {
                                if (ti == dst_ti) { ok = true; break; }
                                if (!SafeReadable(reinterpret_cast<void*>(ti + 16), 8)) break;
                                const uint64_t base = *reinterpret_cast<uint64_t*>(ti + 16);
                                if (base == 0 || !SafeReadable(reinterpret_cast<void*>(base), 16))
                                    break;
                                // Taban gercekten bir type_info mi? Adi okunabilir
                                // bir mangled ASCII string olmali; degilse +16
                                // alani taban degil, alakasiz bellek demektir.
                                const uint64_t nm = *reinterpret_cast<uint64_t*>(base + 8);
                                if (nm == 0 || !SafeReadable(reinterpret_cast<void*>(nm), 1)) break;
                                const char c0 = *reinterpret_cast<char*>(nm);
                                if (c0 < 0x20 || c0 >= 0x7f) break;
                                ti = base;
                            }
                            if (ok) {
                                result = static_cast<uint64_t>(static_cast<int64_t>(sub) - s2d);
                                karar  = "taban-dogrulandi";
                            } else {
                                karar = "null(taban-degil)";
                            }
                        }
                    }
                }

                static std::atomic<uint64_t> s_dc{0};
                const uint64_t dc = s_dc.fetch_add(1) + 1;
                if (dc <= 8 || (dc % 2000ull) == 0) {
                    printf("[DYNCAST] #%llu sub=0x%llx dst_ti=0x%llx src2dst=%lld -> 0x%llx (%s)\n",
                           static_cast<unsigned long long>(dc),
                           static_cast<unsigned long long>(sub),
                           static_cast<unsigned long long>(dst_ti),
                           static_cast<long long>(s2d),
                           static_cast<unsigned long long>(result), karar);
                    fflush(stdout);
                }

                ctx->Rax  = result;
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp); // RET simulasyonu
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // PAD OKUMA - ZINCIRDEN ONCE (girdi yolu)
            // ========================================================
            // Asagidaki dev if/else zincirinde bir "PadReadState" dali VAR ama
            // olcumle kanitlandi ki oraya HIC ULASILMIYOR (dala konan tani
            // satiri bir kez bile basilmadi, oysa [PLT-HLE] PadReadState
            // loglaniyor). Zincirin nerede kesildigini aramak yerine girdi
            // yolunu buraya, zincirin ONUNE aliyoruz: tek sorumlulugu var ve
            // dogru calistigi dogrulanabilir.
            // PadOpen/PadGetHandle de buraya alindi: zincire guvenemedigimiz
            // icin tutamac uretimi ile okuma AYNI yerde olmali. Oyun aksi halde
            // tutamac yerine bir hata kodu tasiyip okumayi reddettiriyordu
            // (olculdu: handle=-2137915391, yani 0x80... Sony hata kodu).
            if (readable_name == "PadOpen" || readable_name == "scePadOpen" ||
                readable_name == "PadGetHandle" || readable_name == "scePadGetHandle") {
                ctx->Rax = 1; // gecerli tutamac; Kyty yalnizca 1'i kabul ediyor
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "PadGetControllerInformation" ||
                readable_name == "scePadGetControllerInformation") {
                void* info = reinterpret_cast<void*>(ctx->Rsi);
                int   rc   = 0;
                if (info != nullptr && SafeWritable(info, 32)) {
                    rc = Libs::Controller::PadGetControllerInformation(
                        1, reinterpret_cast<Libs::Controller::PadControllerInformation*>(info));
                }
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(rc));
                ctx->Rip  = *reinterpret_cast<uint64_t*>(ctx->Rsp);
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (readable_name == "PadReadState" || readable_name == "scePadReadState" ||
                readable_name == "PadRead" || readable_name == "scePadRead") {
                const int guest_handle = static_cast<int>(ctx->Rdi);
                void*     data         = reinterpret_cast<void*>(ctx->Rsi);
                int       rc           = 0;
                // Kyty yalnizca handle==1'i kabul eder. Oyun elinde gecerli bir
                // tutamac olmadan da okumaya calisiyor; emulator olarak bunu
                // reddetmek yerine tek sanal kumandaya yonlendiriyoruz - aksi
                // halde girdi hicbir zaman ulasmiyor.
                const int handle = 1;
                if (data != nullptr && SafeWritable(data, sizeof(Libs::Controller::PadData))) {
                    rc = Libs::Controller::PadReadState(
                        handle, reinterpret_cast<Libs::Controller::PadData*>(data));
                }
                (void)guest_handle;
                static std::atomic<uint64_t> s_pad_reads{0};
                const uint64_t pr  = s_pad_reads.fetch_add(1) + 1;
                const uint32_t btn = (data != nullptr) ? *reinterpret_cast<uint32_t*>(data) : 0u;
                // Buton BASILI oldugu her okumayi yaz. Eskiden yalnizca her
                // 600. okuma yazdiriliyordu (~saniyede bir); bir tus vurusu
                // ~100 ms surdugu icin basimlar neredeyse hep kaciriliyor ve
                // "girdi ulasmiyor" izlenimi veriyordu.
                static std::atomic<int> s_btn_logs{0};
                const bool log_press = (btn != 0) && (s_btn_logs.fetch_add(1) < 40);
                if (pr <= 3 || (pr % 600ull) == 0 || log_press) {
                    printf("[PAD] okuma=%llu misafir_handle=%d data=%p rc=%d butonlar=0x%08x\n",
                           static_cast<unsigned long long>(pr), guest_handle, data, rc, btn);
                    fflush(stdout);
                }
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(rc));
                ctx->Rip = *reinterpret_cast<uint64_t*>(ctx->Rsp); // RET simulasyonu
                ctx->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ========================================================
            // GERCEK BELLEK YONETIMI
            // (sceKernelGetDirectMemorySize / sceKernelAllocateDirectMemory /
            //  sceKernelMapDirectMemory)
            // ========================================================
            // Oyunun kendi C++ runtime'i (thread_local yikici kayit sistemi /
            // "emutls" havuzu dahil - RVA 0x2c61b2 cokmesinin kaynagi) bu
            // ucluye bagli GERCEK bellek olmadan calisamiyor. ABI'ler
            // KytyPS5 (src/libs/libKernel.cpp, src/kernel/memory.h) ve bu
            // ELF'in disassembly'si (fonksiyon 0x2e13a0) ile dogrulandi.
            // Sahte RAX=0 yerine artik gercek VirtualAlloc destekli bellek
            // donduruyoruz.
            bool special_return_set = false;
            
            // RENDER (sceAgc/Graphics/sceVideoOut) cagrilarini psemu'nun eski
            // HLE handler'larindan ONCE Kyty'ye yonlendir. Kyty sahiplenirse
            // (is_render) tum zincir atlanir; boylece render-state TAMAMEN
            // Kyty'de tutarli kalir (yari-psemu/yari-Kyty karisimi olmaz).
            // HIZ: bir PLT'nin AGC/render olup olmadigi SABIT. Eski hali her
            // libc cagrisinda (memset dahil) Agc::Dispatch'e girip substr
            // (heap alloc) + 3 rfind + Kyty NID DB aramasi yapiyordu. Artik
            // karar plt_index basina bir kez veriliyor (0=bilinmiyor, 1=agc,
            // 2=agc-degil); "agc-degil" ise Dispatch hic cagrilmiyor.
            static uint8_t s_agc_state[kPltCacheMax] = {};
            bool try_agc = !(plt_index < kPltCacheMax && s_agc_state[plt_index] == 2);
            bool agc_handled = false;
            if (try_agc) {
                agc_handled = Agc::Dispatch(func_name, readable_name, ctx);
                if (plt_index < kPltCacheMax) {
                    s_agc_state[plt_index] = agc_handled ? 1 : 2;
                }
            }

            if (agc_handled) {
                special_return_set = true;
            } else if (readable_name == "sceKernelCreateEqueue") {
                ctx->Rax = Libs::LibKernel::EventQueue::KernelCreateEqueue((Libs::LibKernel::EventQueue::KernelEqueue*)ctx->Rdi, (const char*)ctx->Rsi);
                special_return_set = true;
            } else if (readable_name == "sceKernelDeleteEqueue") {
                ctx->Rax = Libs::LibKernel::EventQueue::KernelDeleteEqueue((Libs::LibKernel::EventQueue::KernelEqueue)ctx->Rdi);
                special_return_set = true;
            } else if (readable_name == "sceKernelWaitEqueue") {
                ctx->Rax = Libs::LibKernel::EventQueue::KernelWaitEqueue((Libs::LibKernel::EventQueue::KernelEqueue)ctx->Rdi, (Libs::LibKernel::EventQueue::KernelEvent*)ctx->Rsi, (int)ctx->Rdx, (int*)ctx->Rcx, (const Libs::LibKernel::KernelUseconds*)ctx->R8);
                special_return_set = true;
            } else if (readable_name == "sceKernelGetDirectMemorySize") {
                // imza: size_t KernelGetDirectMemorySize()
                // Gercek bir "direct memory" havuzumuz yok; oyunun
                // search_end olarak kullanacagi makul bir ust sinir veriyoruz.
                ctx->Rax = 0x100000000ULL; // 4GB
                special_return_set = true;
            } else if (readable_name == "sceKernelAllocateDirectMemory") {
                // imza: (search_start, search_end, len, alignment, memory_type, *phys_addr_out)
                uint64_t len = ctx->Rdx;
                uint64_t align = ctx->Rcx ? ctx->Rcx : 0x4000;
                static int64_t s_fake_phys_cursor = 0x100000;
                int64_t chosen = (s_fake_phys_cursor + static_cast<int64_t>(align) - 1)
                                 & ~(static_cast<int64_t>(align) - 1);
                s_fake_phys_cursor = chosen + static_cast<int64_t>(len);

                // Fiziksel bellegi TAHSIS aninda commit et. Oyun direct
                // memory'yi tek bitisik alan sayip tahsis ettigi araliga
                // pointer aritmetigiyle dogrudan yaziyor (RVA 0x1132b4'te
                // henuz Map edilmemis bir offset'e yazarken cokuyordu).
                // Tahsis edilen fiziksel bellek "vardir"; Map yalnizca ona
                // bir sanal pencere acar.
                {
                    uint8_t* dbase = DmemBase();
                    uint64_t need  = static_cast<uint64_t>(chosen) + len;
                    if (dbase != nullptr && need <= kDmemSize) {
                        VirtualAlloc(dbase + chosen, static_cast<size_t>(len),
                                     MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    }
                }

                int64_t* out_ptr = reinterpret_cast<int64_t*>(ctx->R9);
                if (ctx->R9 != 0 && SafeWritable(out_ptr, sizeof(int64_t))) {
                    *out_ptr = chosen;
                }
                ctx->Rax = 0; // basari
                special_return_set = true;

                // Gurultu filtresi + ILERLEME olcumu: 818 ayni satir yerine
                // ilk 20'yi, sonra her 100'de bir toplami raporla. Boylece
                // "yavas ama ilerliyor" ile "kacak tahsis" ayirt edilebiliyor.
                {
                    static volatile LONG s_count = 0;
                    LONG n = InterlockedIncrement(&s_count);
                    if (n <= 20 || (n % 100) == 0) {
                        std::stringstream am_ss;
                        am_ss << "[MEMORY-HLE] DirectMemory #" << n
                              << ": len=0x" << std::hex << len
                              << " -> phys=0x" << chosen << std::dec
                              << "  (toplam " << (chosen + len) / (1024 * 1024) << " MB)";
                        LOG_INFO(am_ss.str());
                    }
                }
            // NOT: "Named" varyanti da BURAYA dusmeli. Astro Bot yalnizca
            // sceKernelMapNamedDirectMemory cagiriyor; o ad islenmedigi icin
            // genel stub RAX=0 ("basarili") donuyor ama *addr'a HICBIR SEY
            // yazmiyordu. Oyunun GPU ayirici havuzu boyle tabansiz kaliyor ve
            // 184 MB'lik Onion havuzunda "used 0" gorunmesine ragmen 240 bayt
            // bile bulamiyordu:
            //     ASSERT engine/app/Module/Gpu/GpuMemory.cpp:155
            //     Out of graphics memory [Onion]. size=240, used 0/192937984
            // Tek fark ek bir "name" parametresi (yalnizca hata ayiklama icin).
            } else if (readable_name == "sceKernelMapDirectMemory" ||
                       readable_name == "sceKernelMapNamedDirectMemory") {
                // imza: (void** addr, len, prot, flags, direct_memory_start, alignment)
                uint64_t len = ctx->Rsi;
                uint64_t align = ctx->R9 ? ctx->R9 : 0x4000;
                uint64_t phys = ctx->R8; // direct_memory_start - ONEMLI, yok sayilamaz
                uint64_t flags = ctx->Rcx;
                // addr GIRIS/CIKIS: oyun istedigi sanal adresi verebilir
                // (flags'te FIXED ise zorunlu). Simdilik yalnizca OLCUYORUZ:
                // sifir disi geliyorsa onu onurlandirmamiz gerekiyor demektir.
                uint64_t want_addr = 0;
                if (ctx->Rdi != 0 && SafeReadable(reinterpret_cast<void*>(ctx->Rdi), 8)) {
                    want_addr = *reinterpret_cast<uint64_t*>(ctx->Rdi);
                }
                {
                    static volatile LONG s_wn = 0;
                    if (want_addr != 0 && InterlockedIncrement(&s_wn) <= 10) {
                        std::stringstream ws;
                        ws << "[MEMORY-HLE] Map ISTENEN adres=0x" << std::hex << want_addr
                           << " flags=0x" << flags << " phys=0x" << phys << std::dec
                           << "  <-- onurlandirilmiyor!";
                        LOG_ERROR(ws.str());
                    }
                }
                size_t alloc_size = static_cast<size_t>((len + align - 1) & ~(align - 1));

                // Ayni fiziksel adres -> ayni sanal adres. MEM_COMMIT zaten
                // commit edilmis sayfalari tekrar SIFIRLAMAZ, dolayisiyla
                // ikinci esleme oyunun yazdigi veriyi korur.
                void* mem = nullptr;
                uint8_t* dbase = DmemBase();
                if (dbase != nullptr && phys + alloc_size <= kDmemSize) {
                    mem = VirtualAlloc(dbase + phys, alloc_size,
                                       MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                }
                if (mem == nullptr) {
                    // Havuz disi/basarisiz: eski davranisa dus (en azindan bellek ver)
                    mem = VirtualAlloc(nullptr, alloc_size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                }

                void** out_ptr = reinterpret_cast<void**>(ctx->Rdi);
                if (ctx->Rdi != 0 && SafeWritable(out_ptr, sizeof(void*))) {
                    *out_ptr = mem;
                }
                ctx->Rax = mem ? 0ULL : static_cast<uint64_t>(-1LL);
                special_return_set = true;

                std::stringstream mm_ss;
                {
                    static volatile LONG s_mcount = 0;
                    LONG n = InterlockedIncrement(&s_mcount);
                    if (n <= 20 || (n % 100) == 0) {
                        mm_ss << "[MEMORY-HLE] Map #" << n << ": phys=0x" << std::hex << phys
                              << " len=0x" << len << " -> mapped=0x"
                              << reinterpret_cast<uint64_t>(mem) << std::dec;
                        LOG_INFO(mm_ss.str());
                    }
                }
            } else if (readable_name == "__cxa_guard_acquire") {
                // Itanium C++ ABI: guard'in ilk byte'i "tamamlandi" bayragidir.
                // 0 ise caller HENUZ initialize ETMEMIS demektir - biz simdiye
                // kadar HER ZAMAN RAX=0 dondurerek "zaten tamamlandi, atla"
                // sinyali veriyorduk. Bu, programdaki TUM magic-static (C++11
                // lazy-init statik) objelerinin gercekte HICBIR ZAMAN
                // construct edilmemesine yol aciyordu - RVA 0x2c61b2
                // cokmesindeki G global'i de boyle bir objenin bir alani.
                uint8_t* guard = reinterpret_cast<uint8_t*>(ctx->Rdi);
                if (ctx->Rdi != 0 && SafeReadable(guard, 1)) {
                    ctx->Rax = (*guard == 0) ? 1 : 0;
                } else {
                    ctx->Rax = 1; // guvenli taraf: initialize etmesine izin ver
                }
                special_return_set = true;
            } else if (readable_name == "__cxa_guard_release") {
                // Initialize basariyla tamamlandi; ilk byte'i 1 yapip
                // "tamamlandi" olarak isaretliyoruz.
                uint8_t* guard = reinterpret_cast<uint8_t*>(ctx->Rdi);
                if (ctx->Rdi != 0 && SafeWritable(guard, 1)) {
                    *guard = 1;
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "__cxa_guard_abort") {
                // Initialize sirasinda istisna olustu; guard'i sifirla ki
                // sonraki cagrida tekrar denenebilsin.
                uint8_t* guard = reinterpret_cast<uint8_t*>(ctx->Rdi);
                if (ctx->Rdi != 0 && SafeWritable(guard, 1)) {
                    *guard = 0;
                }
                ctx->Rax = 0;
                special_return_set = true;
            }
            // ========================================================
            // GERCEK C BELLEK YONETIMI (malloc/free/calloc/realloc/...)
            // ========================================================
            // Oyunun libc'si bu fonksiyonlari cagirdiginda simdiye kadar
            // RAX=0 (NULL) donuyorduk; oyun donen NULL pointer'a yazinca
            // coken (ornegin RVA 0x19a2ba, WRITE @ 0x28). Bunlari Windows'un
            // kendi hizali allocator'ina yonlendiriyoruz. PS4/PS5 malloc'u
            // 16-byte hizali doner; _aligned_* ailesi kullanarak free/realloc
            // ile tutarli kaliyoruz (TUM tahsisatlar ayni yoldan gectigi icin
            // free/realloc her zaman _aligned_* pointer'i gorur).
            // C++ global tahsis operatorleri. Astro Bot bunlari libc'den import
            // ediyor (modul tablosu: sonek "#s#s" -> id 44 -> libc) ve NID'leri
            // hicbir veritabaninda yoktu; tuzlu SHA1 hash'iyle kaba kuvvet
            // cozuldu (bkz. tools/scripts/nid_libc.py).
            // malloc/free ile AYNI ayiriciyi kullanmak sart: oyun new ile
            // aldigini free ile, malloc ile aldigini delete ile birakabiliyor.
            else if (readable_name == "_Znwm" || readable_name == "_Znam" ||
                     readable_name == "operator new") {
                const size_t size     = static_cast<size_t>(ctx->Rdi);
                const size_t alloc_sz = size ? (size + 65536) : 65536;
                void*        p        = _aligned_malloc(alloc_sz, 16);
                if (p) memset(p, 0, alloc_sz);
                RegisterAllocSize(p, size);
                ctx->Rax = reinterpret_cast<uint64_t>(p);
                special_return_set = true;
            }
            else if (readable_name == "malloc") {
                // malloc(size): RDI=size
                size_t size = static_cast<size_t>(ctx->Rdi);
                size_t alloc_sz = size ? (size + 65536) : 65536;
                void* p = _aligned_malloc(alloc_sz, 16);
                if (p) memset(p, 0, alloc_sz);
                RegisterAllocSize(p, size); // realloc dogru kopyalayabilsin
                ctx->Rax = reinterpret_cast<uint64_t>(p);
                special_return_set = true;
            } else if (readable_name == "free") {
                // free(ptr): RDI=ptr
                // Serbest birakmayi KASITLI olarak YAPMA. Oyunun dahili
                // allocator'u serbest birakilan bellege hala stale pointer
                // tutuyor olabilir; CRT heap o adresi baska bir tahsise verebilir
                // ve use-after-free / pointer corruption'a yol acabilir.
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "calloc") {
                // calloc(nmemb, size): RDI=nmemb, RSI=size
                size_t nmemb = static_cast<size_t>(ctx->Rdi);
                size_t size = static_cast<size_t>(ctx->Rsi);
                size_t total = nmemb * size;
                size_t alloc_sz = total ? (total + 65536) : 65536;
                void* p = _aligned_malloc(alloc_sz, 16);
                if (p) memset(p, 0, alloc_sz);
                RegisterAllocSize(p, total);
                ctx->Rax = reinterpret_cast<uint64_t>(p);
                special_return_set = true;
            } else if (readable_name == "realloc") {
                // realloc(ptr, size): RDI=ptr, RSI=size
                void* old_p = reinterpret_cast<void*>(ctx->Rdi);
                size_t size = static_cast<size_t>(ctx->Rsi);
                size_t alloc_sz = size ? (size + 65536) : 65536;
                void* p = _aligned_malloc(alloc_sz, 16);
                if (p) {
                    memset(p, 0, alloc_sz);
                    if (old_p != nullptr && SafeReadable(old_p, 1)) {
                        // ESKI HATA: yeni boyut kadar kopyalamaya calisiyordu ve
                        // SafeReadable basarisiz olunca HIC kopyalamiyordu ->
                        // icerik tamamen kayboluyordu (bkz. RegisterAllocSize notu).
                        // Simdi: min(eski_boyut, yeni_boyut); boyut bilinmiyorsa
                        // okunabilen kadarini kopyala (asla "hic" degil).
                        size_t old_sz   = LookupAllocSize(old_p);
                        size_t copy_len = (old_sz != 0 && old_sz < size) ? old_sz : size;
                        if (copy_len == 0) copy_len = 16;
                        while (copy_len > 0 && !SafeReadable(old_p, copy_len)) {
                            copy_len /= 2;
                        }
                        if (copy_len > 0) {
                            memcpy(p, old_p, copy_len);
                        }
                    }
                    RegisterAllocSize(p, size);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(p);
                special_return_set = true;
            } else if (readable_name == "memalign") {
                // memalign(alignment, size): RDI=alignment, RSI=size
                size_t align = static_cast<size_t>(ctx->Rdi);
                size_t size = static_cast<size_t>(ctx->Rsi);
                if (align < 16 || (align & (align - 1)) != 0) align = 16;
                size_t alloc_sz = size ? (size + 65536) : 65536;
                void* p = _aligned_malloc(alloc_sz, align);
                if (p) memset(p, 0, alloc_sz);
                RegisterAllocSize(p, size);
                ctx->Rax = reinterpret_cast<uint64_t>(p);
                special_return_set = true;
            } else if (readable_name == "posix_memalign") {
                // posix_memalign(memptr, alignment, size): RDI=memptr, RSI=align, RDX=size
                void** memptr = reinterpret_cast<void**>(ctx->Rdi);
                size_t align = static_cast<size_t>(ctx->Rsi);
                size_t size = static_cast<size_t>(ctx->Rdx);
                if (align < 16 || (align & (align - 1)) != 0) align = 16;
                size_t alloc_sz = size ? (size + 65536) : 65536;
                void* p = _aligned_malloc(alloc_sz, align);
                if (p) memset(p, 0, alloc_sz);
                if (memptr != nullptr && SafeWritable(memptr, sizeof(void*))) {
                    *memptr = p;
                }
                ctx->Rax = p ? 0 : 12; // basari=0, hata=ENOMEM(12)
                special_return_set = true;
            }
            // ========================================================
            // GERCEK BELLEK KOPYALAMA/DOLDURMA (memset/memcpy/memmove)
            // ========================================================
            // Bunlar da simdiye kadar NO-OP (RAX=0) idi; yani oyunun
            // "sifirladigini"/"kopyaladigini" sandigi bellek aslinda hic
            // dokunulmuyordu -> sessiz veri bozulmasi. Gercekten yapiyoruz
            // ve C standardina gore hedef pointer'i (RDI) donduruyoruz.
            else if (readable_name == "memset") {
                // memset(dst, c, n): RDI=dst, RSI=c, RDX=n
                void* dst = reinterpret_cast<void*>(ctx->Rdi);
                int c = static_cast<int>(ctx->Rsi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                // NOT: IsBadWritePtr VEH icinde guvenilmez (SEH probe'u bize
                // dusuyor); buyuk n degerlerinde yanlis "basarisiz" deyip
                // islemi SESSIZCE atlayabiliyordu. SafeReadable kullaniyoruz.
                if (dst && n && SafeWritable(dst, n)) {
                    memset(dst, c, n);
                }
                ctx->Rax = ctx->Rdi; // memset hedef pointer'i doner
                special_return_set = true;
            } else if (readable_name == "memcpy") {
                // memcpy(dst, src, n): RDI=dst, RSI=src, RDX=n
                void* dst = reinterpret_cast<void*>(ctx->Rdi);
                void* src = reinterpret_cast<void*>(ctx->Rsi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                bool ok = false;
                if (dst && src && n && SafeReadable(src, n) && SafeWritable(dst, n)) {
                    memcpy(dst, src, n);
                    ok = true;
                }
                // Tani: buyuk kopyalar (data.js gibi) tam mi, kesiliyor mu?
                if (n > 65536) {
                    static int s_n = 0;
                    if (s_n < 8) {
                        s_n++;
                        std::stringstream mc;
                        mc << "[VFS] BUYUK memcpy #" << s_n << ": " << n << " byte"
                           << (ok ? " (yapildi)" : " (ATLANDI - guvenli degil!)");
                        LOG_INFO(mc.str());
                    }
                }
                ctx->Rax = ctx->Rdi; // memcpy hedef pointer'i doner
                special_return_set = true;
            } else if (readable_name == "memmove") {
                // memmove(dst, src, n): RDI=dst, RSI=src, RDX=n
                void* dst = reinterpret_cast<void*>(ctx->Rdi);
                void* src = reinterpret_cast<void*>(ctx->Rsi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                bool mv_ok = false;
                if (dst && src && n && SafeReadable(src, n) && SafeWritable(dst, n)) {
                    memmove(dst, src, n);
                    mv_ok = true;
                }
                if (n > 65536) {
                    static int s_n = 0;
                    if (s_n < 8) {
                        s_n++;
                        std::stringstream mm;
                        mm << "[VFS] BUYUK memmove #" << s_n << ": " << n << " byte"
                           << (mv_ok ? " (yapildi)" : " (ATLANDI - guvenli degil!)");
                        LOG_INFO(mm.str());
                    }
                }
                ctx->Rax = ctx->Rdi; // memmove hedef pointer'i doner
                special_return_set = true;
            }
            // ========================================================
            // GERCEK STRING FONKSIYONLARI (str*)
            // ========================================================
            // KRITIK: Bunlar da NO-OP idi (RAX=0). Oyunun TUM string islemleri
            // sessizce hicbir sey yapmiyordu; ornegin romPathPrefix'i kuran
            // strcpy/strcat zinciri bos bir yol uretiyor, fopen "/" aliyor ve
            // "romPathPrefix must end with a slash!" ile exit(1) ediliyordu.
            else if (readable_name == "strlen") {
                // Sinirsiz: data.js gibi 1MB+ tamponlar kesilmemeli.
                size_t n = SafeStrlen(reinterpret_cast<const char*>(ctx->Rdi));
                // Tani: buyuk tamponlarda uzunlugun TAM dogru oldugunu gorelim
                // ve tamponun basindaki birkac byte'i (BOM var mi?) raporlayalim.
                if (n > 100000) {
                    static int s_n = 0;
                    if (s_n < 5) {
                        s_n++;
                        const uint8_t* b = reinterpret_cast<const uint8_t*>(ctx->Rdi);
                        std::stringstream sl;
                        sl << "[VFS] strlen(buyuk) #" << s_n << " -> " << n << " byte";
                        if (SafeReadable(b, 4)) {
                            char hx[32];
                            snprintf(hx, sizeof(hx), "  ilk baytlar: %02X %02X %02X %02X",
                                     b[0], b[1], b[2], b[3]);
                            sl << hx;
                            if (b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) sl << "  [UTF-8 BOM]";
                        }
                        LOG_INFO(sl.str());
                        // Tamponun SONU saglam mi? (parse erken bitiyorsa burada
                        // bozulma/kesilme gorunur)
                        if (n >= 80 && SafeReadable(b + n - 80, 80)) {
                            std::string tail(reinterpret_cast<const char*>(b + n - 80), 80);
                            std::string clean;
                            for (char c : tail) clean += isprint((unsigned char)c) ? c : '.';
                            LOG_INFO("[VFS]   tampon SONU (son 80 byte): " + clean);
                        }
                    }
                }
                ctx->Rax = static_cast<uint64_t>(n);
                special_return_set = true;
            } else if (readable_name == "strnlen") {
                size_t maxn = static_cast<size_t>(ctx->Rsi);
                std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi), maxn);
                ctx->Rax = static_cast<uint64_t>(s.size() < maxn ? s.size() : maxn);
                special_return_set = true;
            } else if (readable_name == "strcpy") {
                char* dst = reinterpret_cast<char*>(ctx->Rdi);
                std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi));
                if (SafeWritable(dst, s.size() + 1)) { memcpy(dst, s.c_str(), s.size() + 1); }
                ctx->Rax = ctx->Rdi;
                special_return_set = true;
            } else if (readable_name == "strncpy") {
                char* dst = reinterpret_cast<char*>(ctx->Rdi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi), n);
                if (n > 0 && SafeWritable(dst, n)) {
                    size_t c = s.size() < n ? s.size() : n;
                    memcpy(dst, s.data(), c);
                    if (c < n) memset(dst + c, 0, n - c); // strncpy kalani sifirlar
                }
                ctx->Rax = ctx->Rdi;
                special_return_set = true;
            } else if (readable_name == "strcat") {
                char* dst = reinterpret_cast<char*>(ctx->Rdi);
                std::string d = SafeReadCString(dst);
                std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi));
                if (SafeWritable(dst, d.size() + s.size() + 1)) {
                    memcpy(dst + d.size(), s.c_str(), s.size() + 1);
                }
                ctx->Rax = ctx->Rdi;
                special_return_set = true;
            } else if (readable_name == "strncat") {
                char* dst = reinterpret_cast<char*>(ctx->Rdi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                std::string d = SafeReadCString(dst);
                std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi), n);
                {
                    size_t c = s.size() < n ? s.size() : n;
                    if (SafeWritable(dst, d.size() + c + 1)) {
                        memcpy(dst + d.size(), s.data(), c);
                        dst[d.size() + c] = 0;
                    }
                }
                ctx->Rax = ctx->Rdi;
                special_return_set = true;
            } else if (readable_name == "strcmp" || readable_name == "strcasecmp") {
                std::string a = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi));
                std::string b = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi));
                if (readable_name == "strcasecmp") {
                    for (auto& ch : a) ch = static_cast<char>(tolower((unsigned char)ch));
                    for (auto& ch : b) ch = static_cast<char>(tolower((unsigned char)ch));
                }
                int r = strcmp(a.c_str(), b.c_str());
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                special_return_set = true;
            } else if (readable_name == "strncmp") {
                size_t n = static_cast<size_t>(ctx->Rdx);
                std::string a = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi), n);
                std::string b = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi), n);
                int r = strncmp(a.c_str(), b.c_str(), n);
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                special_return_set = true;
            } else if (readable_name == "memcmp" || readable_name == "bcmp") {
                const void* a = reinterpret_cast<const void*>(ctx->Rdi);
                const void* b = reinterpret_cast<const void*>(ctx->Rsi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                int r = 0;
                g_memcmp_bytes.fetch_add(n, std::memory_order_relaxed);
                uint64_t prev_max = g_memcmp_max.load(std::memory_order_relaxed);
                while (n > prev_max &&
                       !g_memcmp_max.compare_exchange_weak(prev_max, n,
                                                           std::memory_order_relaxed)) {}
                if (n && SafeReadable(a, n) && SafeReadable(b, n)) r = memcmp(a, b, n);
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                special_return_set = true;
            } else if (readable_name == "strchr" || readable_name == "strrchr") {
                const char* s = reinterpret_cast<const char*>(ctx->Rdi);
                int c = static_cast<int>(ctx->Rsi);
                std::string str = SafeReadCString(s);
                size_t pos = (readable_name == "strchr") ? str.find(static_cast<char>(c))
                                                         : str.rfind(static_cast<char>(c));
                // c==0 ise sonlandiriciyi gosterir
                if (c == 0) ctx->Rax = ctx->Rdi + str.size();
                else ctx->Rax = (pos == std::string::npos) ? 0 : (ctx->Rdi + pos);
                special_return_set = true;
            } else if (readable_name == "__cxa_throw") {
                // __cxa_throw(void* exc, type_info*, void(*dest)(void*))
                // Gercek C++ exception yayilimi (unwinding) desteklenmiyor;
                // ama exception nesnesinin ICINDEKI MESAJI okuyup raporlayabiliriz.
                // std::runtime_error benzeri nesnelerde +0 vtable, ardindan
                // string (ya dogrudan ya da pointer olarak) bulunur.
                uint8_t* exc = reinterpret_cast<uint8_t*>(ctx->Rdi);
                std::stringstream th;
                th << "[EXCEPTION] Oyun C++ exception firlatti. exc=0x"
                   << std::hex << ctx->Rdi << std::dec;
                LOG_ERROR(th.str());

                // CALL CHAIN'i EN BASTA bas (sonraki eski tanilardan biri
                // cokse bile yolu gorelim): __cxa_throw'un cagiran zinciri.
                {
                    std::stringstream ec;
                    ec << "[EXCEPTION] yol (RVA, RBP-zinciri):";
                    uint64_t rbp = ctx->Rbp;
                    for (int i = 0; i < 20; i++) {
                        if (!SafeReadable(reinterpret_cast<void*>(rbp), 16)) break;
                        uint64_t saved = *reinterpret_cast<uint64_t*>(rbp);
                        uint64_t ret   = *reinterpret_cast<uint64_t*>(rbp + 8);
                        if (ret >= g_base_addr && ret < g_base_addr + g_module_size)
                            ec << " 0x" << std::hex << (ret - g_base_addr) << std::dec;
                        if (saved <= rbp) break;
                        rbp = saved;
                    }
                    LOG_ERROR(ec.str());
                }

                // GERCEK BOZUK STRING VERISINI HEAP'TE BUL (bolum-bolum tarama).
                // invalid_utf16 nesnesi (RDI+8) bozuk birimden SONRAKI birimi
                // ("igne") saklar. Bellekte [lead-surrogate][igne] 4-bayt
                // desenini DOGRUDAN arariz -> gercek u16 char verisini bulur
                // (pointer baytlarina takilan eski taramanin aksine). Bulunca
                // ONCESI ve SONRASI baytlari da dokup surrogate'in eklenmis bir
                // karakter mi yoksa cop mu oldugunu gosteririz.
                if (ctx->Rsi != 0 && SafeReadable(reinterpret_cast<void*>(ctx->Rsi + 8), 8)) {
                    const char* tn0 = *reinterpret_cast<const char**>(ctx->Rsi + 8);
                    bool is_utf16 = tn0 && SafeReadable(tn0, 16) &&
                                    strstr(tn0, "invalid_utf16") != nullptr;
                    uint16_t needle = 0;
                    if (SafeReadable(reinterpret_cast<void*>(ctx->Rdi + 8), 2))
                        needle = *reinterpret_cast<uint16_t*>(ctx->Rdi + 8);
                    if (is_utf16 && needle != 0) {
                        int dumped = 0;
                        uint64_t scanned_mb = 0;
                        uint8_t* addr = nullptr;
                        MEMORY_BASIC_INFORMATION mbi;
                        while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                               dumped < 4 && scanned_mb < 768) {
                            uint8_t* base = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                            size_t rsz = mbi.RegionSize;
                            addr = base + rsz; // sonraki bolge
                            // Yalnizca commit + yazilabilir + makul boyut (dev
                            // dmem havuzunu/kod'u atla)
                            DWORD pr = mbi.Protect & 0xFF;
                            bool rw = (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE);
                            if (mbi.State != MEM_COMMIT || !rw || (mbi.Protect & PAGE_GUARD)) continue;
                            if (rsz > 64u * 1024 * 1024) continue; // dev bellek havuzu vb.
                            scanned_mb += rsz / (1024 * 1024) + 1;
                            const uint16_t* u = reinterpret_cast<const uint16_t*>(base);
                            size_t n = rsz / 2;
                            for (size_t k = 1; k + 1 < n; k++) {
                                if (u[k] >= 0xD800 && u[k] <= 0xDBFF && u[k + 1] == needle) {
                                    uint64_t at = reinterpret_cast<uint64_t>(u + k);
                                    // ONCESI 20 birim + SONRASI: baglami goster
                                    size_t start = k >= 20 ? k - 20 : 0;
                                    std::stringstream hs;
                                    hs << "[UTF16-BULUNDU] @0x" << std::hex << at
                                       << " (surrogate 0x" << u[k] << " + igne 0x" << needle
                                       << ") baglam u16:" << std::dec;
                                    for (size_t m = start; m < k + 40 && m < n; m++)
                                        hs << " " << std::hex << std::setw(4) << std::setfill('0') << u[m];
                                    LOG_ERROR(hs.str());
                                    std::stringstream as; as << "[UTF16-BULUNDU] ascii: ";
                                    for (size_t m = start; m < k + 60 && m < n; m++) {
                                        uint16_t c = u[m];
                                        as << (char)((c >= 32 && c < 127) ? c : '.');
                                    }
                                    LOG_ERROR(as.str());
                                    if (++dumped >= 4) break;
                                    k += 40;
                                }
                            }
                        }
                        if (dumped == 0)
                            LOG_ERROR("[UTF16-BULUNDU] desen heap'te bulunamadi "
                                      "(igne 0x" + [](uint16_t v){std::stringstream x;x<<std::hex<<v;return x.str();}(needle) + ")");
                    }
                }
                // type_info (RSI) -> sinif adi. std::type_info: [0]=vtable,
                // [8]=mangled isim (char*). Mesaj bulunamasa bile sinif bize
                // exception'in NE oldugunu soyler (ornegin nlohmann type_error).
                if (ctx->Rsi != 0 && SafeReadable(reinterpret_cast<void*>(ctx->Rsi + 8), 8)) {
                    const char* tn = *reinterpret_cast<const char**>(ctx->Rsi + 8);
                    if (tn && SafeReadable(tn, 1)) {
                        std::string name = SafeReadCString(tn, 200);
                        if (!name.empty())
                            LOG_ERROR("[EXCEPTION]   tip (mangled): " + name);
                    }
                }

                if (exc && SafeReadable(exc, 0x30)) {
                    // Ham nesne dokumu: invalid_utf16 bozuk kod birimini
                    // (+0x08'de uint16) saklar; deger cop mu, belirli bir
                    // desen mi gormek icin ilk 0x18 bayti bas.
                    {
                        std::stringstream hx;
                        hx << "[EXCEPTION]   exc ham:";
                        for (int i = 0; i < 0x18; i++)
                            hx << " " << std::hex << std::setw(2) << std::setfill('0')
                               << (int)exc[i];
                        uint16_t u16 = *reinterpret_cast<uint16_t*>(exc + 8);
                        hx << std::dec << "   (u16@+8 = 0x" << std::hex << u16 << std::dec << ")";
                        LOG_ERROR(hx.str());
                    }
                    // Nesnenin ilk 6 qword'unu tarayip string'e benzeyenleri bas
                    for (int i = 0; i < 6; i++) {
                        uint64_t v = *reinterpret_cast<uint64_t*>(exc + i * 8);
                        if (v == 0) continue;
                        const char* sp = reinterpret_cast<const char*>(v);
                        if (SafeReadable(sp, 1)) {
                            std::string s = SafeReadCString(sp, 256);
                            // Yazdirilabilir ve anlamli uzunlukta ise mesajdir
                            bool printable = !s.empty();
                            for (char c : s) {
                                if (!isprint(static_cast<unsigned char>(c)) && c != '\n') {
                                    printable = false; break;
                                }
                            }
                            if (printable && s.size() >= 4) {
                                LOG_ERROR("[EXCEPTION]   +0x" + std::to_string(i * 8)
                                          + " -> \"" + s + "\"");
                            }
                        }
                    }
                    // Kucuk-string optimizasyonu (SSO): nesnenin icinde gomulu olabilir
                    std::string inl = SafeReadCString(reinterpret_cast<const char*>(exc + 8), 40);
                    bool ok = !inl.empty();
                    for (char c : inl) {
                        if (!isprint(static_cast<unsigned char>(c))) { ok = false; break; }
                    }
                    if (ok && inl.size() >= 4) {
                        LOG_ERROR("[EXCEPTION]   gomulu(SSO) -> \"" + inl + "\"");
                    }
                }
                // Cagri zinciri: RBP-zinciri gezerek GERCEK caller'lari bul.
                // NOT: Onceki surum RSP'den itibaren stack'i ham taruyordu -
                // bu, gercek donus adresleri yerine cop/spill degerleri
                // yakalayabiliyordu (dogrulandi: bircok "adres" ayni kucuk
                // fonksiyona dusuyor ve call-site tablosunda hic yok).
                // Bu binary push rbp; mov rbp,rsp cercevesi koruyor, o
                // yuzden klasik frame-pointer walk guvenilir sonuc verir.
                {
                    std::stringstream bt;
                    bt << "[EXCEPTION] cagri zinciri (RVA, RBP-zinciri):";
                    uint64_t rbp = ctx->Rbp;
                    for (int i = 0; i < 16; i++) {
                        if (!SafeReadable(reinterpret_cast<void*>(rbp), 16)) break;
                        uint64_t saved_rbp = *reinterpret_cast<uint64_t*>(rbp);
                        uint64_t ret_addr  = *reinterpret_cast<uint64_t*>(rbp + 8);
                        if (ret_addr < g_base_addr || ret_addr >= g_base_addr + g_module_size) break;
                        bt << " 0x" << std::hex << (ret_addr - g_base_addr) << std::dec;
                        if (saved_rbp <= rbp) break; // cerceve zinciri kirilmis/dongu
                        rbp = saved_rbp;
                    }
                    LOG_ERROR(bt.str());
                }
                // ERISILEN JSON NESNESINI DOK: parse'in sadik oldugu kanitlandigina
                // gore hata, oyunun okudugu nesnenin bekledigimiz yapi OLMAMASINDA.
                // nlohmann basic_json yerlesimi: +0 tip baytÄ±, +8 deger/pointer.
                // Dizi ise +8 -> std::vector{begin,end,cap}, eleman boyutu 16.
                {
                    auto dump_json = [&](const char* label, uint64_t addr) {
                        if (!SafeReadable(reinterpret_cast<void*>(addr), 16)) {
                            LOG_ERROR(std::string("[EXCEPTION]   ") + label + ": okunamadi");
                            return;
                        }
                        uint8_t  t   = *reinterpret_cast<uint8_t*>(addr);
                        uint64_t val = *reinterpret_cast<uint64_t*>(addr + 8);
                        static const char* kNames[] = {"null","object","array","string",
                                                       "boolean","int","uint","float","discarded"};
                        std::stringstream ds;
                        ds << "[EXCEPTION]   " << label << ": tip=" << (int)t
                           << " (" << (t < 9 ? kNames[t] : "?") << ")";
                        ds << "  payload=0x" << std::hex << val << std::dec;
                        if (t == 2 && val != 0) {
                            if (!SafeReadable(reinterpret_cast<void*>(val), 24)) {
                                ds << "  (vector okunamadi)";
                            } else {
                                uint64_t b = *reinterpret_cast<uint64_t*>(val);
                                uint64_t e = *reinterpret_cast<uint64_t*>(val + 8);
                                ds << "  begin=0x" << std::hex << b
                                   << " end=0x" << e << std::dec;
                                if (e >= b && b != 0) {
                                    uint64_t n = (e - b) / 16;
                                    ds << "  ELEMAN=" << n << "  icerik:";
                                    for (uint64_t i = 0; i < n && i < 10; i++) {
                                        uint64_t ea = b + i * 16;
                                        if (!SafeReadable(reinterpret_cast<void*>(ea), 16)) break;
                                        uint8_t  et = *reinterpret_cast<uint8_t*>(ea);
                                        uint64_t ev = *reinterpret_cast<uint64_t*>(ea + 8);
                                        if (et == 5 || et == 6) {
                                            ds << " " << static_cast<int64_t>(ev);
                                        } else if (et == 7) {
                                            double dv; std::memcpy(&dv, &ev, 8);
                                            ds << " " << dv << "f";
                                        } else if (et < 9) {
                                            static const char* kN[] = {"null","obj","arr","str",
                                                                       "bool","i","u","f","disc"};
                                            ds << " <" << kN[et] << ">";
                                        } else {
                                            ds << " <?" << (int)et << ">";
                                        }
                                    }
                                }
                            }
                        }
                        LOG_ERROR(ds.str());
                    };
                    // DOGRU CERCEVEYI BUL: RBP zincirini gez; donus adresi
                    // 0x1e3cf7 (getter cagrisinin donusu) olan cercevenin BIR
                    // USTU, JSON yerellerini tutan 0x1e23c0'in cercevesidir.
                    // (Onceki deneme yanlis cercevede aradigi icin cop okudu.)
                    uint64_t frame_1e23c0 = 0;
                    {
                        uint64_t rbp = ctx->Rbp;
                        for (int i = 0; i < 16; i++) {
                            if (!SafeReadable(reinterpret_cast<void*>(rbp), 16)) break;
                            uint64_t saved = *reinterpret_cast<uint64_t*>(rbp);
                            uint64_t ret   = *reinterpret_cast<uint64_t*>(rbp + 8);
                            // OYUNA OZEL tani: 0x1e3cf7/0x1423bf adresleri
                            // Dreaming Sarah'in JSON getter cercevesine ait.
                            if (Game::Current().quirk_rva_diagnostics &&
                                ret == g_base_addr + 0x1e3cf7) {
                                frame_1e23c0 = saved;
                                break;
                            }
                            if (saved <= rbp) break;
                            rbp = saved;
                        }
                    }
                    if (frame_1e23c0 != 0) {
                        // CERCEVE DOGRULAMA: dogruysa [frame+8] = 0x1423bf olmali
                        if (SafeReadable(reinterpret_cast<void*>(frame_1e23c0 + 8), 8)) {
                            uint64_t r = *reinterpret_cast<uint64_t*>(frame_1e23c0 + 8);
                            std::stringstream fv;
                            fv << "[EXCEPTION]   cerceve dogrulama: donus RVA=0x" << std::hex
                               << (r - g_base_addr) << std::dec
                               << (r == g_base_addr + 0x1423bf ? "  (DOGRU)" : "  (YANLIS CERCEVE!)");
                            LOG_ERROR(fv.str());
                        }
                        // Ham bayt dokumu: payload'in gercekte ne oldugunu gorelim
                        auto hexdump = [&](const char* label, uint64_t addr, int n) {
                            if (!SafeReadable(reinterpret_cast<void*>(addr), n)) return;
                            std::stringstream hs;
                            hs << "[EXCEPTION]   " << label << " @0x" << std::hex << addr << ":";
                            for (int i = 0; i < n; i += 8) {
                                hs << " " << *reinterpret_cast<uint64_t*>(addr + i);
                            }
                            LOG_ERROR(hs.str());
                        };
                        uint64_t ja = frame_1e23c0 - 0x80;
                        if (SafeReadable(reinterpret_cast<void*>(ja), 32)) {
                            uint64_t pl = *reinterpret_cast<uint64_t*>(ja + 8);
                            hexdump("eleman json ham (32b)", ja, 32);
                            if (pl) {
                                hexdump("payload ham (128b)", pl, 128);
                                // Vector'u HEM payload+0 HEM payload+8'den deneyip
                                // hangisi tutarli eleman sayisi veriyorsa onu coz.
                                for (int off = 0; off <= 8; off += 8) {
                                    if (!SafeReadable(reinterpret_cast<void*>(pl + off), 16)) continue;
                                    uint64_t bg = *reinterpret_cast<uint64_t*>(pl + off);
                                    uint64_t en = *reinterpret_cast<uint64_t*>(pl + off + 8);
                                    if (bg == 0 || en < bg) continue;
                                    uint64_t cnt = (en - bg) / 16;
                                    if (cnt == 0 || cnt > 64) continue;
                                    std::stringstream es;
                                    es << "[EXCEPTION]   vector@payload+" << off
                                       << "  ELEMAN=" << cnt << ":";
                                    for (uint64_t i = 0; i < cnt && i < 14; i++) {
                                        uint64_t ea = bg + i * 16;
                                        if (!SafeReadable(reinterpret_cast<void*>(ea), 16)) break;
                                        uint8_t  t2 = *reinterpret_cast<uint8_t*>(ea);
                                        uint64_t v2 = *reinterpret_cast<uint64_t*>(ea + 8);
                                        es << "  [" << i << "]";
                                        if (t2 == 5 || t2 == 6) {
                                            es << static_cast<int64_t>(v2);
                                        } else if (t2 == 7) {
                                            double dv; std::memcpy(&dv, &v2, 8);
                                            es << dv << "f";
                                        } else if (t2 < 9) {
                                            static const char* kN[] = {"NULL","obj","arr","str",
                                                                       "bool","i","u","f","disc"};
                                            es << "<" << kN[t2] << ">";
                                        } else {
                                            es << "<?" << (int)t2 << ">";
                                        }
                                    }
                                    LOG_ERROR(es.str());
                                    // IC ICE: [10] float dizisi olmali
                                    // ([-0.28125,-0.78125,...]). Oradaki float'lar
                                    // da null ise kayip SISTEMATIK demektir.
                                    if (cnt > 10) {
                                        uint64_t e10 = bg + 10 * 16;
                                        uint8_t  t10 = *reinterpret_cast<uint8_t*>(e10);
                                        uint64_t p10 = *reinterpret_cast<uint64_t*>(e10 + 8);
                                        if (t10 == 2 && p10 &&
                                            SafeReadable(reinterpret_cast<void*>(p10 + 8), 16)) {
                                            uint64_t b3 = *reinterpret_cast<uint64_t*>(p10 + 8);
                                            uint64_t e3 = *reinterpret_cast<uint64_t*>(p10 + 16);
                                            if (b3 && e3 >= b3) {
                                                uint64_t c3 = (e3 - b3) / 16;
                                                std::stringstream is;
                                                is << "[EXCEPTION]   ic dizi [10] ELEMAN=" << c3 << ":";
                                                for (uint64_t k = 0; k < c3 && k < 16; k++) {
                                                    uint64_t ka = b3 + k * 16;
                                                    if (!SafeReadable(reinterpret_cast<void*>(ka), 16)) break;
                                                    uint8_t  tk = *reinterpret_cast<uint8_t*>(ka);
                                                    uint64_t vk = *reinterpret_cast<uint64_t*>(ka + 8);
                                                    if (tk == 7) {
                                                        double dk; std::memcpy(&dk, &vk, 8);
                                                        is << " " << dk << "f";
                                                    } else if (tk == 5 || tk == 6) {
                                                        is << " " << static_cast<int64_t>(vk);
                                                    } else if (tk == 0) {
                                                        is << " NULL";
                                                    } else {
                                                        is << " <t" << (int)tk << ">";
                                                    }
                                                }
                                                LOG_ERROR(is.str());
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        dump_json("okunan eleman [0x1e23c0 rbp-0x80]", frame_1e23c0 - 0x80);
                        dump_json("kapsayici    [0x1e23c0 rbp-0xc0]", frame_1e23c0 - 0xc0);
                        // Dongu sayaci ve siniri da oku: kacinci elemanda coktuk?
                        uint64_t cnt_a = frame_1e23c0 - 0x100;
                        uint64_t cnt_b = frame_1e23c0 - 0x110;
                        if (SafeReadable(reinterpret_cast<void*>(cnt_b), 0x20)) {
                            std::stringstream lc;
                            lc << "[EXCEPTION]   dongu: sayac[rbp-0x100]="
                               << *reinterpret_cast<int64_t*>(cnt_a)
                               << " sinir[rbp-0x110]="
                               << *reinterpret_cast<int64_t*>(cnt_b);
                            LOG_ERROR(lc.str());
                        }
                    } else {
                        // Getter'lar (0x100400 / 0x11f500) girste "mov rbx, rdi"
                        // yapiyor ve RBX callee-saved; okunan JSON orada.
                        // Tip bayti 0-8 disindaysa nlohmann type_name() "number"
                        // dondugu icin mesaj "but is number" gorunur.
                        dump_json("RBX (okunan json)", ctx->Rbx);
                        std::stringstream rs;
                        rs << "[EXCEPTION]   RSP=0x" << std::hex << ctx->Rsp
                           << "  RBP=0x" << ctx->Rbp
                           << "  yigin kullanimi=" << std::dec
                           << ((ctx->Rbp > ctx->Rsp) ? (ctx->Rbp - ctx->Rsp) : 0) << " byte";
                        LOG_ERROR(rs.str());
                    }
                }
                // PARSE SADAKAT KONTROLU: data.js'te 153913 tamsayi + 12795 float
                // var. Sayilar tutuyorsa ayristirma dosyaya sadik demektir ve
                // hatayi baska yerde aramaliyiz; tutmuyorsa sapma var.
                {
                    std::stringstream pc;
                    pc << "[EXCEPTION] parse sadakati: tamsayi=" << g_n_strtoint
                       << " (data.js'te 153913)  float=" << g_n_strtod
                       << " (data.js'te 12795)";
                    LOG_ERROR(pc.str());
                }
                // Donersek cagri sonrasi ud2'ye duseriz; anlamli bir mesajla bitir.
                LOG_ERROR("[EXCEPTION] Unwinding desteklenmedigi icin emulasyon burada duruyor.");
                fflush(stdout);
                ExitProcess(1);
            } else if (readable_name == "operator_new") {
                // C++ operator new(size_t): RDI=size -> bellek pointer'i.
                // 0 donmek RVA 0x12ed2d'de "mov [rbx],rax" ile NULL-write
                // cokmesine yol aciyordu. malloc ile ayni hizali havuzu kullan.
                size_t size = static_cast<size_t>(ctx->Rdi);
                void* p = _aligned_malloc(size ? size : 1, 16);
                if (p) memset(p, 0, size ? size : 1);
                ctx->Rax = reinterpret_cast<uint64_t>(p);
                special_return_set = true;
            } else if (readable_name == "strtol" || readable_name == "strtoll" ||
                       readable_name == "strtoul" || readable_name == "strtoull") {
                // long/long long/unsigned strtoX(const char* nptr, char** endptr, int base)
                //   RDI=nptr, RSI=endptr, RDX=base
                // ONEMLI: endptr DOGRU ilerletilmezse cagiran "hic karakter
                // tuketilmedi = gecersiz sayi" anlar. nlohmann tam olarak
                // bunu yapiyor (endptr != beklenen_son -> parse basarisiz ->
                // discarded -> json null) - data.js'in null gelmesinin sebebi buydu.
                const char* nptr = reinterpret_cast<const char*>(ctx->Rdi);
                char** endptr    = reinterpret_cast<char**>(ctx->Rsi);
                int base         = static_cast<int>(ctx->Rdx);
                bool is_unsigned = (readable_name == "strtoul" || readable_name == "strtoull");
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_n_strtoint));
                // JSON sayilari kisa; buyuk tamponun ortasindan okuyoruz, NUL'a
                // kadar gitmesin diye siniri makul tutuyoruz.
                std::string s = SafeReadCString(nptr, 256);
                char* end = nullptr;
                uint64_t raw = 0;
                errno = 0;
                if (!s.empty()) {
                    if (is_unsigned) {
                        raw = static_cast<uint64_t>(strtoull(s.c_str(), &end, base));
                    } else {
                        raw = static_cast<uint64_t>(strtoll(s.c_str(), &end, base));
                    }
                }
                if (errno == ERANGE) g_guest_errno = kGuestERANGE;
                if (endptr != nullptr && SafeReadable(endptr, 8)) {
                    // endptr GUEST string'ine isaret etmeli: ilerlenen kadar kaydir
                    size_t consumed = (end != nullptr) ? static_cast<size_t>(end - s.c_str()) : 0;
                    *endptr = const_cast<char*>(nptr) + consumed;
                }
                ctx->Rax = raw;
                special_return_set = true;
            } else if (readable_name == "powf") {
                // float powf(float x, float y): XMM0=x, XMM1=y -> XMM0
                float x, y;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                std::memcpy(&y, &ctx->Xmm1.Low, sizeof(y));
                float r = powf(x, y);
                uint64_t low = 0; std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low = low; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "ldexpf") {
                // float ldexpf(float x, int exp): XMM0=x, EDI=exp -> XMM0
                float x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                int e = static_cast<int>(static_cast<int32_t>(ctx->Rdi));
                float r = ldexpf(x, e);
                uint64_t low = 0; std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low = low; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "cosf" || readable_name == "logf" ||
                       readable_name == "log2f" || readable_name == "expf" ||
                       readable_name == "sqrtf" || readable_name == "fabsf" ||
                       readable_name == "floorf" || readable_name == "ceilf") {
                // Tek-argumanli float->float matematik (XMM0 giris/cikis).
                // cosf/logf su an TAHMIN; digerleri gerekirse hazir dursun.
                float x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                float r;
                if      (readable_name == "cosf")   r = cosf(x);
                else if (readable_name == "logf")   r = logf(x);
                else if (readable_name == "log2f")  r = log2f(x);
                else if (readable_name == "expf")   r = expf(x);
                else if (readable_name == "sqrtf")  r = sqrtf(x);
                else if (readable_name == "fabsf")  r = fabsf(x);
                else if (readable_name == "floorf") r = floorf(x);
                else                                r = ceilf(x);
                uint64_t low = 0; std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low = low; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "sinf") {
                // float sinf(float x): XMM0 giris, XMM0 cikis
                float x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                float r = sinf(x);
                uint64_t low = 0;
                std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low  = low;
                ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "sincosf") {
                // void sincosf(float x, float* s, float* c)
                //   XMM0=x, RDI=s, RSI=c
                float x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                float s = sinf(x);
                float c = cosf(x);
                float* sp = reinterpret_cast<float*>(ctx->Rdi);
                float* cp = reinterpret_cast<float*>(ctx->Rsi);
                if (sp && SafeWritable(sp, sizeof(float))) *sp = s;
                if (cp && SafeWritable(cp, sizeof(float))) *cp = c;
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "cos" || readable_name == "sin" ||
                       readable_name == "tan" || readable_name == "asin" ||
                       readable_name == "acos" || readable_name == "atan" ||
                       readable_name == "exp" || readable_name == "log10" ||
                       readable_name == "log2" || readable_name == "cbrt" ||
                       readable_name == "round") {
                // DOUBLE tek-argumanli matematik: XMM0 giris, XMM0 cikis.
                // eboot bunlari import ediyor ama eslenmemislerdi -> stub RAX=0
                // donup XMM0'i cop birakiyordu (C2 animasyon/konum matematigi
                // bozuluyordu; log10 render-loop esiginde spin ediyordu).
                double x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                double r;
                if      (readable_name == "cos")   r = cos(x);
                else if (readable_name == "sin")   r = sin(x);
                else if (readable_name == "tan")   r = tan(x);
                else if (readable_name == "asin")  r = asin(x);
                else if (readable_name == "acos")  r = acos(x);
                else if (readable_name == "atan")  r = atan(x);
                else if (readable_name == "exp")   r = exp(x);
                else if (readable_name == "log10") r = log10(x);
                else if (readable_name == "log2")  r = log2(x);
                else if (readable_name == "cbrt")  r = cbrt(x);
                else                               r = round(x);
                uint64_t bits = 0; std::memcpy(&bits, &r, sizeof(r));
                ctx->Xmm0.Low = bits; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "pow" || readable_name == "fmod") {
                // double f(double x, double y): XMM0=x, XMM1=y -> XMM0
                double x, y;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                std::memcpy(&y, &ctx->Xmm1.Low, sizeof(y));
                double r = (readable_name == "pow") ? pow(x, y) : fmod(x, y);
                uint64_t bits = 0; std::memcpy(&bits, &r, sizeof(r));
                ctx->Xmm0.Low = bits; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "roundf") {
                // float roundf(float x): XMM0 giris/cikis
                float x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                float r = roundf(x);
                uint64_t low = 0; std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low = low; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                special_return_set = true;
            } else if (readable_name == "sincos") {
                // void sincos(double x, double* s, double* c): XMM0=x, RDI=s, RSI=c
                double x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                double s = sin(x);
                double c = cos(x);
                double* sp = reinterpret_cast<double*>(ctx->Rdi);
                double* cp = reinterpret_cast<double*>(ctx->Rsi);
                if (sp && SafeWritable(sp, sizeof(double))) *sp = s;
                if (cp && SafeWritable(cp, sizeof(double))) *cp = c;
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "fp_isfinite") {
                // int isfinite(double x): XMM0'daki degeri denetler.
                // ONEMLI - donus mantigi (RVA 0x12dbdf'ten dogrulandi):
                //   jne 0x12e434  -> SIFIR DEGILSE degeri KORU
                //   fallthrough   -> SIFIRSA dugumu NULL'a cevir:
                //                    mov byte [rax],0 / mov qword [rax+8],0
                // Yani gecerli (sonlu) sayi icin SIFIR-DISI donmeli.
                // Bu implement edilmeden once stub RAX=0 donduruyordu; oyun
                // da bu yuzden data.js'teki 12795 float'in HEPSINI null'ladi.
                double x;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                bool finite = !(std::isnan(x) || std::isinf(x));
                {
                    static int s_n = 0;
                    if (s_n < 10) {
                        s_n++;
                        std::stringstream fs;
                        fs << "[fp_check #" << s_n << "] XMM0=" << x;
                        uint64_t np = ctx->Rbp - 0xe8;
                        if (SafeReadable(reinterpret_cast<void*>(np), 8)) {
                            uint64_t node = *reinterpret_cast<uint64_t*>(np);
                            if (node && SafeReadable(reinterpret_cast<void*>(node), 16)) {
                                uint8_t  t = *reinterpret_cast<uint8_t*>(node);
                                uint64_t v = *reinterpret_cast<uint64_t*>(node + 8);
                                double   d; std::memcpy(&d, &v, 8);
                                fs << "  dugum tip=" << (int)t << " deger=" << d
                                   << (t == 7 ? "  [SAKLANDI]" : "  [SAKLANMADI!]");
                            }
                        }
                        LOG_INFO(fs.str());
                    }
                }
                ctx->Rax = finite ? 1 : 0;
                special_return_set = true;
            } else if (readable_name == "strtod" || readable_name == "strtof") {
                // double strtod(const char* nptr, char** endptr) -> sonuc XMM0'da
                //   RDI=nptr, RSI=endptr
                const char* nptr = reinterpret_cast<const char*>(ctx->Rdi);
                char** endptr    = reinterpret_cast<char**>(ctx->Rsi);
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_n_strtod));
                std::string s = SafeReadCString(nptr, 256);
                char* end = nullptr;
                errno = 0;
                double val = s.empty() ? 0.0 : strtod(s.c_str(), &end);
                if (errno == ERANGE) g_guest_errno = kGuestERANGE;
                if (endptr != nullptr && SafeReadable(endptr, 8)) {
                    size_t consumed = (end != nullptr) ? static_cast<size_t>(end - s.c_str()) : 0;
                    *endptr = const_cast<char*>(nptr) + consumed;
                }
                // SysV ABI: kayan nokta donusu XMM0'in dusuk 64 bitinde.
                // ContextFlags'e FLOATING_POINT eklenmezse XMM yazimi CPU'ya
                // geri yansimayabilir; garantiye aliyoruz.
                uint64_t bits;
                std::memcpy(&bits, &val, sizeof(bits));
                ctx->Xmm0.Low  = bits;
                ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                // TANI: cagiran endptr'i token sonuyla karsilastiriyor; tutmazsa
                // degeri HIC saklamiyor (json null kaliyor). Ilk cagrilari dok.
                {
                    static int s_n = 0;
                    if (s_n < 10) {
                        s_n++;
                        size_t consumed = (end != nullptr)
                                        ? static_cast<size_t>(end - s.c_str()) : 0;
                        std::string shown = s.substr(0, 24);
                        std::string clean;
                        for (char c : shown) clean += isprint((unsigned char)c) ? c : '.';
                        std::stringstream sd;
                        sd << "[strtod #" << s_n << "] girdi=\"" << clean
                           << "\" tuketilen=" << consumed
                           << " deger=" << val
                           << std::hex
                           << "  nptr=0x" << ctx->Rdi
                           << "  yazdigimiz_endptr=0x"
                           << (ctx->Rdi + consumed)
                           << "  beklenen(RBX)=0x" << ctx->Rbx;
                        // GERI OKU: yazma gercekten gerceklesti mi?
                        if (endptr != nullptr && SafeReadable(endptr, 8)) {
                            sd << "  GERI_OKUNAN=0x"
                               << reinterpret_cast<uint64_t>(*endptr);
                        } else {
                            sd << "  [ENDPTR YAZILAMADI!]";
                        }
                        // HEDEF DUGUM: parser [rbp-0xe8]'deki json'a yaziyor.
                        // PLT stub cerceve kurmadigi icin RBP hala parser'in.
                        // Onceki cagrinin yazimi tuttuysa burada tip=7 gorurUz.
                        uint64_t np = ctx->Rbp - 0xe8;
                        if (SafeReadable(reinterpret_cast<void*>(np), 8)) {
                            uint64_t node = *reinterpret_cast<uint64_t*>(np);
                            sd << "  hedef_dugum=0x" << node;
                            if (node && SafeReadable(reinterpret_cast<void*>(node), 16)) {
                                sd << " (su anki tip=" << std::dec
                                   << (int)*reinterpret_cast<uint8_t*>(node) << std::hex << ")";
                            }
                        }
                        sd << std::dec
                           << ((ctx->Rdi + consumed) == ctx->Rbx ? "  [hesap ESLESTI]"
                                                                 : "  [hesap ESLESMEDI!]");
                        LOG_INFO(sd.str());
                    }
                }
                special_return_set = true;
            } else if (readable_name == "__error") {
                // BSD libc: int* __error(void) -> errno'nun ADRESINI dondurur.
                // 0 donmek oyunun "*__error() = 0" yazmasiyla RVA 0x12db16'da
                // NULL-write cokmesine yol aciyordu. Her thread'e kendi errno'su.
                // strtoull/strtoll/strtod ile AYNI degiskeni paylasmali:
                // cagiran once *__error()=0 yapip cagri sonrasi ERANGE bakiyor.
                ctx->Rax = reinterpret_cast<uint64_t>(&g_guest_errno);
                special_return_set = true;
            } else if (readable_name == "memchr") {
                // memchr(s, c, n): RDI=s, RSI=c, RDX=n -> bulunan adres veya NULL
                const uint8_t* s = reinterpret_cast<const uint8_t*>(ctx->Rdi);
                int c = static_cast<int>(ctx->Rsi & 0xFF);
                size_t n = static_cast<size_t>(ctx->Rdx);
                uint64_t found = 0;
                if (s && n && SafeReadable(s, n)) {
                    const void* p = memchr(s, c, n);
                    if (p) found = reinterpret_cast<uint64_t>(p);
                }
                ctx->Rax = found;
                special_return_set = true;
            } else if (func_name.find("Noj9PsJrsa8") != std::string::npos) {
                // char_traits<char16_t>::copy / move (16-bit strings)
                uint16_t* dest = (uint16_t*)ctx->Rdi;
                const uint16_t* src = (const uint16_t*)ctx->Rsi;
                size_t n = (size_t)ctx->Rdx;
                if (dest && src && n > 0 && SafeReadable(src, n * 2) && SafeWritable(dest, n * 2)) {
                    memmove(dest, src, n * 2);
                }
                ctx->Rax = (uint64_t)dest;
                special_return_set = true;
            } else if (func_name.find("fnUEjBCNRVU") != std::string::npos) {
                // char_traits<char16_t>::find benzeri: (s=RDI, c=RSI, n=RDX), 2 byte.
                // ONCEDEN handler YOKTU -> default stub RAX=0 (NULL) donuyordu;
                // GameMaker '|'(0x7c) ayracini bulamayip menu string parse dongusunde
                // DONUYORDU (birkac saniyelik freeze) + butonlar bos kaliyordu.
                // n = ELEMAN sayisi (char16_t). Kardes fn Noj9PsJrsa8 char16_t oldugu
                // + item aciklamalari u16string oldugu icin 2 byte.
                const uint16_t* p = reinterpret_cast<const uint16_t*>(ctx->Rdi);
                uint16_t        c = static_cast<uint16_t>(ctx->Rsi);
                size_t          n = static_cast<size_t>(ctx->Rdx);
                // TANI: hangi string'ler '|' ile split ediliyor? Menu etiketleri
                // data.js'te tek string olarak duruyor ("New Game|Continue|Options|
                // ?????|Quit") ve bu fonksiyonla parcalaniyor. Icerigi ASCII'ye
                // dokup menu string'inin buraya HIC gelip gelmedigini goruyoruz.
                { static std::atomic<uint64_t> s_d{0};
                  uint64_t cnt = s_d.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (p != nullptr && n > 0 && SafeReadable(p, (n < 48 ? n : 48) * 2)) {
                    char txt[52]; size_t m = (n < 48 ? n : 48), k = 0;
                    for (size_t i = 0; i < m; i++) {
                        uint16_t ch = p[i];
                        txt[k++] = (ch >= 0x20 && ch < 0x7f) ? static_cast<char>(ch) : '.';
                    }
                    txt[k] = '\0';
                    // Ornekleme (ilk 20 + her 200) YETMIYOR: menu anahtari lang0.json'da
                    // 96. girdi (~split #384) ve ornekleme araligina dusmuyordu.
                    // "menu"/"New game" iceren HER split'i mutlaka yaz -> loadDictionary
                    // menu0'i gercekten isliyor mu, kesin gorelim.
                    const bool is_menu = (strstr(txt, "menu") != nullptr) ||
                                         (strstr(txt, "New game") != nullptr);
                    if (cnt <= 20 || cnt % 200 == 0 || is_menu) {
                        printf("[U16SPLIT] #%llu c=0x%x n=%zu \"%s\"%s\n",
                               static_cast<unsigned long long>(cnt), c, n, txt,
                               is_menu ? "   <== MENU!" : "");
                        fflush(stdout);
                    }
                  } }
                uint64_t found = 0;
                if (p != nullptr && n > 0 && SafeReadable(p, n * 2)) {
                    for (size_t i = 0; i < n; i++) {
                        if (p[i] == c) { found = reinterpret_cast<uint64_t>(&p[i]); break; }
                    }
                }
                ctx->Rax = found;
                special_return_set = true;
            } else if (func_name.find("QJ5xVfKkni0") != std::string::npos) {
                // char_traits<char16_t>::compare(s1, s2, n): (RDI=s1, RSI=s2, RDX=n), 2 byte.
                // find('|') sonrasi bulunan pozisyonu ayrac pattern'iyle karsilastirmak
                // icin cagriliyor. Handler YOKTU -> stub RAX=0 (=esit) donuyordu ->
                // GameMaker parse'i yanlis ilerleyip ayni string'leri tekrar isleyerek
                // DONUYORDU. Donus: 0=esit, <0 s1<s2, >0 s1>s2.
                const uint16_t* s1 = reinterpret_cast<const uint16_t*>(ctx->Rdi);
                const uint16_t* s2 = reinterpret_cast<const uint16_t*>(ctx->Rsi);
                size_t          n  = static_cast<size_t>(ctx->Rdx);
                { static int s_c = 0; if (s_c < 4 && s1 && s2 &&
                     SafeReadable(s1, n * 2) && SafeReadable(s2, n * 2)) { s_c++;
                  const uint8_t* a = reinterpret_cast<const uint8_t*>(s1);
                  const uint8_t* b = reinterpret_cast<const uint8_t*>(s2);
                  printf("[U16CMP] n=%zu s1=%02x %02x %02x %02x s2=%02x %02x %02x %02x\n",
                         n, a[0],a[1],a[2],a[3], b[0],b[1],b[2],b[3]); fflush(stdout); } }
                int result = 0;
                if (s1 && s2 && n > 0 && SafeReadable(s1, n * 2) && SafeReadable(s2, n * 2)) {
                    for (size_t i = 0; i < n; i++) {
                        if (s1[i] != s2[i]) { result = (s1[i] < s2[i]) ? -1 : 1; break; }
                    }
                }
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(result));
                special_return_set = true;
            } else if (func_name.find("sUP1hBaouOw") != std::string::npos) {
                // _Getpctype(): CRT'nin KARAKTER TIPI TABLOSUNU dondurur.
                // tolower/toupper/isalpha bu tabloyu kullanir:
                //     tolower(c) ~ (table[c] & _UPPER) ? c+32 : c
                // ONCEDEN "locales" dalinda RAX=0 (NULL) donuyordu. Tablo NULL
                // olunca (VEH commit-on-fault sifir sayfa acar) tum siniflandirma
                // bitleri 0 okunur -> tolower() karakteri DEGISTIRMEDEN dondurur
                // -> Construct2'nin buyuk/kucuk harf duyarsiz fonksiyon adi
                // eslemesi BOZULUR: Call("menuReload") tanimi "MenuReload" olan
                // fonksiyonu bulamaz -> menu etiketleri hic yazilmaz.
                // MSVC duzeni: dizi [-1..255], isaretci indeks 0'i gosterir.
                {
                    static uint16_t s_ctype[257];
                    static bool     s_init = false;
                    if (!s_init) {
                        s_init = true;
                        constexpr uint16_t U = 0x0001, L = 0x0002, D = 0x0004, S = 0x0008,
                                           P = 0x0010, C = 0x0020, B = 0x0040, X = 0x0080,
                                           A = 0x0100; // _ALPHA
                        s_ctype[0] = 0; // EOF girdisi
                        for (int ch = 0; ch < 256; ch++) {
                            uint16_t v = 0;
                            if (ch < 0x20 || ch == 0x7f)                 v |= C;
                            if (ch == ' ')                               v |= S | B;
                            if (ch == '\t')                              v |= S | B;
                            if (ch=='\n'||ch=='\v'||ch=='\f'||ch=='\r')  v |= S;
                            if (ch >= '0' && ch <= '9')                  v |= D | X;
                            if (ch >= 'A' && ch <= 'Z')                  v |= U | A;
                            if (ch >= 'a' && ch <= 'z')                  v |= L | A;
                            if ((ch>='A'&&ch<='F')||(ch>='a'&&ch<='f'))  v |= X;
                            if (ch > 0x20 && ch < 0x7f &&
                                !(v & (D | U | L)))                      v |= P;
                            s_ctype[ch + 1] = v;
                        }
                    }
                    ctx->Rax = reinterpret_cast<uint64_t>(&s_ctype[1]); // indeks 0
                }
                special_return_set = true;
            } else if (func_name.find("kALvdgEv5ME") != std::string::npos || func_name.find("9nf8joUTSaQ") != std::string::npos || func_name.find("rcQCUr0EaRU") != std::string::npos || func_name.find("p6LrHjIQMdk") != std::string::npos || func_name.find("hqi8yMOCmG0") != std::string::npos || func_name.find("QW2jL1J5rwY") != std::string::npos || func_name.find("P8F2oavZXtY") != std::string::npos || func_name.find("Q1BL70XVV0o") != std::string::npos) {
                // _Locksyslock / _Unlocksyslock / locales / exceptions
                ctx->Rax = 0;
                special_return_set = true;
            } else if (func_name.find("T72hz6ffq08") != std::string::npos) {
                // scePthreadYield
                SwitchToThread();
                ctx->Rax = 0;
                special_return_set = true;
            } else if (func_name.find("pKwslsMUmSk") != std::string::npos) {
                // fmod(double x, double y): XMM0=x, XMM1=y -> XMM0
                double x = 0, y = 0;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                std::memcpy(&y, &ctx->Xmm1.Low, sizeof(y));
                double r = (y != 0.0) ? std::fmod(x, y) : 0.0;
                uint64_t low = 0; std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low = low; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                ctx->Rax = 0;
                special_return_set = true;
            } else if (func_name.find("9LCjpWyQ5Zc") != std::string::npos) {
                // pow(double x, double y): XMM0=x, XMM1=y -> XMM0
                double x = 0, y = 0;
                std::memcpy(&x, &ctx->Xmm0.Low, sizeof(x));
                std::memcpy(&y, &ctx->Xmm1.Low, sizeof(y));
                double r = std::pow(x, y);
                uint64_t low = 0; std::memcpy(&low, &r, sizeof(r));
                ctx->Xmm0.Low = low; ctx->Xmm0.High = 0;
                ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "clock_gettime" || readable_name == "sceKernelClockGettime") {
                // int clock_gettime(clockid_t clk_id, struct timespec* tp)
                //   RDI=clk_id, RSI=tp
                // FreeBSD timespec: { int64 tv_sec; int64 tv_nsec; } = 16 byte
                // CLOCK_REALTIME=0, CLOCK_MONOTONIC=4
                int      clk = static_cast<int>(ctx->Rdi);
                int64_t* tp  = reinterpret_cast<int64_t*>(ctx->Rsi);
                uint64_t ns  = (clk == 0) ? RealtimeNs() : MonotonicNs();
                if (tp != nullptr && SafeReadable(tp, 16)) {
                    tp[0] = static_cast<int64_t>(ns / 1000000000ull); // tv_sec
                    tp[1] = static_cast<int64_t>(ns % 1000000000ull); // tv_nsec
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "gettimeofday" || readable_name == "sceKernelGettimeofday") {
                // int gettimeofday(struct timeval* tv, struct timezone* tz)
                //   RDI=tv, RSI=tz
                // FreeBSD timeval: { int64 tv_sec; int64 tv_usec; } = 16 byte
                int64_t* tv = reinterpret_cast<int64_t*>(ctx->Rdi);
                uint64_t ns = RealtimeNs();
                if (tv != nullptr && SafeReadable(tv, 16)) {
                    tv[0] = static_cast<int64_t>(ns / 1000000000ull);          // tv_sec
                    tv[1] = static_cast<int64_t>((ns % 1000000000ull) / 1000);  // tv_usec
                }
                // timezone alani kullanilmiyorsa dokunmuyoruz (NULL olabilir)
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "libc_time") {
                // time_t time(time_t* t): RDI=t (NULL olabilir) -> saniye doner
                int64_t secs = static_cast<int64_t>(RealtimeNs() / 1000000000ull);
                int64_t* out = reinterpret_cast<int64_t*>(ctx->Rdi);
                if (out != nullptr && SafeReadable(out, 8)) *out = secs;
                ctx->Rax = static_cast<uint64_t>(secs);
                special_return_set = true;
            } else if (readable_name == "sceKernelGetProcessTime") {
                // GERCEK API: uint64_t sceKernelGetProcessTime(void)
                //   -> ARGUMAN ALMAZ, mikrosaniyeyi RAX'ta DONDURUR.
                // Onceki hali degeri *RDI'ye yazip RAX=0 donuyordu. Iki sorun:
                //  1) Oyun donus degerini okudugu icin her zaman 0 aliyordu ->
                //     delta/elapsed = 0. Oyun yuklemeyi kare basina ZAMAN BUTCESI
                //     ile yaptigi icin ilerleme neredeyse durmustu (185 saniyede
                //     ~20 diyalog girdisi; lang0.json'da 636 girdi var). Menu
                //     metinleri de bu yuzden hic olusmuyor: oyun hala yukluyor.
                //  2) Fonksiyon argumansiz oldugundan RDI COPTUR; oraya 8 bayt
                //     yazmak alakasiz bellegi bozabilir.
                // HER IKI SOZLESME: degeri RAX'ta dondur (gercek API) VE RDI
                // gecerli/yazilabilir gorunuyorsa oraya da yaz. Bazi
                // sarmalayicilar isaretci bicimini bekliyor olabilir; C2
                // runtime'inin "Wait" zamanlayicisi bu saate bagli oldugu icin
                // yanlis bicim beklemeleri sonsuza kilitler.
                {
                    uint64_t us = MonotonicNs() / 1000ull;
                    ctx->Rax    = us;
                    uint64_t* out = reinterpret_cast<uint64_t*>(ctx->Rdi);
                    if (out != nullptr && ctx->Rdi > 0x10000ULL && SafeWritable(out, 8)) {
                        *out = us;
                    }
                }
                special_return_set = true;
            } else if (readable_name == "sceKernelGetProcessTimeCounter") {
                // GERCEK API: uint64_t sceKernelGetProcessTimeCounter(void)
                // (argumansiz, sayaci RAX'ta dondurur - yukaridaki ile ayni gerekce)
                {
                    LARGE_INTEGER now; QueryPerformanceCounter(&now);
                    uint64_t v = static_cast<uint64_t>(now.QuadPart);
                    ctx->Rax   = v;
                    uint64_t* out = reinterpret_cast<uint64_t*>(ctx->Rdi);
                    if (out != nullptr && ctx->Rdi > 0x10000ULL && SafeWritable(out, 8)) {
                        *out = v;
                    }
                }
                special_return_set = true;
            } else if (readable_name == "sceKernelReadTsc") {
                // uint64_t sceKernelReadTsc(void) -> returns tick count in RAX
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                ctx->Rax = static_cast<uint64_t>(now.QuadPart);
                special_return_set = true;
            } else if (readable_name == "sceKernelGetProcessTimeCounterFrequency" ||
                       readable_name == "sceKernelGetTscFrequency") {
                ctx->Rax = static_cast<uint64_t>(g_qpc_freq.QuadPart);
                special_return_set = true;
            } else if (readable_name == "sceKernelUsleep") {
                // sceKernelUsleep(microseconds): RDI=us. Gercekten uyu ki
                // oyunun bekleme dongusu CPU'yu doldurmasin.
                uint64_t us = ctx->Rdi;
                DWORD ms = static_cast<DWORD>(us / 1000);
                Sleep(ms ? ms : 1);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "strstr") {
                std::string h = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi));
                std::string nd = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi));
                size_t pos = h.find(nd);
                ctx->Rax = (pos == std::string::npos) ? 0 : (ctx->Rdi + pos);
                special_return_set = true;
            } else if (readable_name == "libc_char_table") {
                // Karakter donusum tablosu dondurur; kod table[char*2] okuyup
                // sonucu dogrudan cikti string'ine yaziyor (RVA 0x2b622e).
                // Gercek tablo icerigi bilinmedigi icin IDENTITY kuruyoruz:
                // table[c*2] = c  -> non-ASCII karakterler DEGISMEDEN gecer.
                // (Sifir birakmak tum non-ASCII karakterleri yok ederdi.)
                static uint8_t* s_char_table = nullptr;
                if (s_char_table == nullptr) {
                    s_char_table = reinterpret_cast<uint8_t*>(
                        VirtualAlloc(nullptr, 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                    if (s_char_table) {
                        for (int i = 0; i < 256; i++) s_char_table[i * 2] = static_cast<uint8_t>(i);
                    }
                }
                ctx->Rax = reinterpret_cast<uint64_t>(s_char_table);
                special_return_set = true;
            }
            // ========================================================
            // GERCEK DOSYA I/O (VFS): /app0/... -> oyun klasoru
            // ========================================================
            // Oyun "/app0/~INDEX", "/app0/save_data_icon.png" gibi dosyalari
            // aciyor. fopen NULL dondugu icin "no VFS rom ~INDEX" diyordu.
            // Artik gercek host dosyalarina baglaniyoruz.
            else if (readable_name == "SaveDataMount3" || readable_name == "sceSaveDataMount" ||
                     func_name.find("ZP4e7rlzOUk") != std::string::npos) {
                // SaveDataMount3(mount, result): RDI=mount, RSI=result
                void* res_ptr = reinterpret_cast<void*>(ctx->Rsi);
                // sceSaveDataMount3: RDI=mount, RSI=mountResult, RDX=err?
                // mountResult contains mountPoint (char[16]) and requiredBlocks (uint64_t).
                // If we don't initialize it, the game reads uninitialized requiredBlocks and tries to allocate a huge buffer, crashing the allocator.
                void* mountResult = reinterpret_cast<void*>(ctx->Rsi);
                if (mountResult && SafeWritable(mountResult, 64)) {
                    memset(mountResult, 0, 64);
                    strcpy(reinterpret_cast<char*>(mountResult), "/saveData0");
                }
                LOG_INFO("[SaveData] SaveDataMount3 -> SUCCESS (0) [/saveData0]");
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "SaveDataUmount2" || readable_name == "sceSaveDataUmount" ||
                       func_name.find("uW4vfTwMQVo") != std::string::npos) {
                ctx->Rax = 0;
                special_return_set = true;
                LOG_INFO("[SaveData] SaveDataUmount2 -> SUCCESS (0)");
            } else if (readable_name == "sceKernelOpen" || readable_name == "libc_open" ||
                       func_name.find("1G3lF1Gg1k8") != std::string::npos) {
                // sceKernelOpen(path, flags, mode): RDI=path, RSI=flags, RDX=mode
                std::string guest = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi));
                // Gercek dosyayi VFS ile bulmaya calis
                std::string host = TranslateGuestPath(guest);
                FILE* f = nullptr;
                // ONCEDEN: bayraklar YOK SAYILIP her zaman "rb" ile aciliyordu.
                // Oyun kayit dosyasini (O_CREAT|O_WRONLY) OLUSTURAMIYOR, surekli
                // -1 aliyor ve "/saveData0/-saveindex" asamasinda takiliyordu.
                // FreeBSD/PS bayraklari: O_WRONLY=1 O_RDWR=2 O_APPEND=0x8
                //                        O_CREAT=0x200 O_TRUNC=0x400
                {
                    const uint32_t flags  = static_cast<uint32_t>(ctx->Rsi);
                    const uint32_t acc    = flags & 3u;
                    const bool     creat  = (flags & 0x200u) != 0;
                    const bool     trunc  = (flags & 0x400u) != 0;
                    const bool     append = (flags & 0x8u) != 0;
                    const char*    m      = "rb";
                    if (acc == 1u)      m = append ? "ab"  : (trunc || creat ? "wb"  : "r+b");
                    else if (acc == 2u) m = append ? "a+b" : (trunc         ? "w+b" : "r+b");
                    if (!host.empty()) {
                        if (creat || acc != 0u) {
                            EnsureParentDirs(host); // kayit klasoru yoksa olustur
                        }
                        f = fopen(host.c_str(), m);
                        // r+b istendi ama dosya yoksa ve O_CREAT verilmisse olustur
                        if (f == nullptr && creat && acc != 0u) {
                            f = fopen(host.c_str(), acc == 2u ? "w+b" : "wb");
                        }
                    }
                }
                if (f) {
                    // Gercek dosya var: fd olarak FILE* saklayip dondur
                    static int s_fd_counter = 100;
                    int fd = s_fd_counter++;
                    {
                        std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                        // fd -> FILE* esleme tablosu
                        static std::unordered_map<int, FILE*>& fd_map = *new std::unordered_map<int, FILE*>();
                        fd_map[fd] = f;
                    }
                    ctx->Rax = static_cast<uint64_t>(fd);
                    special_return_set = true;
                    LOG_INFO("[KernelIO] sceKernelOpen(\"" + guest + "\") -> fd=" + std::to_string(fd) + " (gercek dosya)");
                } else {
                    // Dosya yok: -1 (ENOENT) dondur ki oyun "dosya bulunamadi" yoluna gitsin
                    ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(-1));
                    special_return_set = true;
                    LOG_INFO("[KernelIO] sceKernelOpen(\"" + guest + "\") -> -1 (ENOENT: dosya bulunamadi)");
                }
            } else if (readable_name == "sceKernelRead" || readable_name == "libc_read" ||
                       func_name.find("Cg4srZ6TKbU") != std::string::npos) {
                // sceKernelRead(fd, buf, nbyte): RDI=fd, RSI=buf, RDX=nbyte
                int fd = static_cast<int>(ctx->Rdi);
                void* buf = reinterpret_cast<void*>(ctx->Rsi);
                size_t nbyte = static_cast<size_t>(ctx->Rdx);
                FILE* f = nullptr;
                {
                    std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                    static std::unordered_map<int, FILE*>& fd_map = *new std::unordered_map<int, FILE*>();
                    if (fd_map.count(fd)) f = fd_map[fd];
                }
                size_t bytes_read = 0;
                if (f && buf && nbyte > 0 && SafeWritable(buf, nbyte)) {
                    bytes_read = fread(buf, 1, nbyte, f);
                } else if (buf && nbyte > 0 && SafeWritable(buf, nbyte)) {
                    memset(buf, 0, nbyte);
                    bytes_read = nbyte;
                }
                ctx->Rax = static_cast<uint64_t>(bytes_read);
                special_return_set = true;
            } else if (readable_name == "sceKernelWrite" || readable_name == "libc_write") {
                // sceKernelWrite(fd, buf, nbyte): RDI=fd, RSI=buf, RDX=nbyte
                size_t nbyte = static_cast<size_t>(ctx->Rdx);
                ctx->Rax = static_cast<uint64_t>(nbyte);
                special_return_set = true;
            } else if (readable_name == "sceKernelClose" || readable_name == "libc_close" ||
                       func_name.find("UK2Tl2DWUns") != std::string::npos) {
                // sceKernelClose(fd): RDI=fd
                int fd = static_cast<int>(ctx->Rdi);
                {
                    std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                    static std::unordered_map<int, FILE*>& fd_map = *new std::unordered_map<int, FILE*>();
                    if (fd_map.count(fd)) {
                        fclose(fd_map[fd]);
                        fd_map.erase(fd);
                    }
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "sceKernelStat" || readable_name == "libc_stat" ||
                       func_name.find("eV9wAD2riIA") != std::string::npos) {
                // sceKernelStat(path, buf): RDI=path, RSI=buf
                void* sb = reinterpret_cast<void*>(ctx->Rsi);
                if (sb && SafeWritable(sb, 128)) {
                    memset(sb, 0, 128);
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "fopen") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                std::string guest = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi));
                std::string mode  = SafeReadCString(reinterpret_cast<const char*>(ctx->Rsi));
                if (mode.empty()) mode = "rb";
                if (mode.find('b') == std::string::npos) mode += "b"; // her zaman binary
                std::string host = TranslateGuestPath(guest);
                // Yazma/ekleme modunda hedef dizin yoksa olustur (kayit verisi).
                if (mode.find('w') != std::string::npos || mode.find('a') != std::string::npos ||
                    mode.find('+') != std::string::npos) {
                    EnsureParentDirs(host);
                }
                FILE* f = fopen(host.c_str(), mode.c_str());
                if (f) { g_open_files.insert(f); g_open_names[f] = guest; }
                ctx->Rax = reinterpret_cast<uint64_t>(f);
                special_return_set = true;

                std::stringstream fo;
                fo << "[VFS] fopen(\"" << guest << "\", \"" << mode << "\") -> \"" << host
                   << "\" : " << (f ? "ACILDI" : "BASARISIZ");
                LOG_INFO(fo.str());
            } else if (readable_name == "fclose") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                int r = -1;
                if (g_open_files.count(f)) { r = fclose(f); g_open_files.erase(f); g_open_names.erase(f); }
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                special_return_set = true;
            } else if (readable_name == "fread") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                // fread(ptr, size, count, stream): RDI,RSI,RDX,RCX
                void* ptr = reinterpret_cast<void*>(ctx->Rdi);
                size_t sz = static_cast<size_t>(ctx->Rsi);
                size_t cnt = static_cast<size_t>(ctx->Rdx);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rcx);
                size_t got = 0;
                bool known = g_open_files.count(f) != 0;
                // fread hedefe sz*cnt bayt YAZIYOR; tum araligi dogrula.
                // Yalnizca ilk bayti kontrol etmek, buyuk dokularin commit
                // edilmis bolgeyi asip WRITE violation uretmesine yol aciyordu.
                size_t want_bytes = (sz && cnt > SIZE_MAX / sz) ? 0 : sz * cnt;
                if (known && want_bytes && SafeWritable(ptr, want_bytes)) {
                    got = fread(ptr, sz, cnt, f);
                }
                // VFS'e ozel tani (PLT log filtresinden bagimsiz): buyuk
                // okumalarin TAM gelip gelmedigini gormek icin.
                {
                    static int s_n = 0;
                    size_t total = sz * cnt;
                    // Ilk 20'yi ve HER buyuk okumayi (>64KB) logla; data.js gibi
                    // buyuk dosyalar sinire takilip gorunmez olmasin.
                    // AYRICA: .json (lokalizasyon: lang0.json) okumalari HER ZAMAN
                    // gorunsun - menu metinleri bu dosyadan geliyor ve dosya
                    // kucuk oldugu icin yukaridaki iki kosula takilmiyordu.
                    auto it_nm = g_open_names.find(f);
                    const bool is_json = (it_nm != g_open_names.end() &&
                                          it_nm->second.find(".json") != std::string::npos);
                    static std::atomic<int> s_json_n{0};
                    if (s_n < 20 || total > 65536 ||
                        (is_json && s_json_n.fetch_add(1, std::memory_order_relaxed) < 16)) {
                        s_n++;
                        auto it = g_open_names.find(f);
                        std::stringstream fr;
                        fr << "[VFS] fread #" << s_n << " ["
                           << (it != g_open_names.end() ? it->second : "?") << "]"
                           << ": istenen=" << total << " byte -> okunan=" << got
                           << (known ? "" : "  [BILINMEYEN FILE*]")
                           << (known && got != cnt ? "  <-- EKSIK!" : "");
                        LOG_INFO(fr.str());
                    }
                }
                ctx->Rax = static_cast<uint64_t>(got);
                special_return_set = true;
            } else if (readable_name == "fwrite") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                // fwrite(ptr, size, count, stream) - stdout/stderr'e giderse logla
                const char* ptr = reinterpret_cast<const char*>(ctx->Rdi);
                size_t sz = static_cast<size_t>(ctx->Rsi);
                size_t cnt = static_cast<size_t>(ctx->Rdx);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rcx);
                size_t total = sz * cnt;
                if (g_open_files.count(f)) {
                    ctx->Rax = static_cast<uint64_t>(fwrite(ptr, sz, cnt, f));
                } else {
                    // Bilinmeyen stream = muhtemelen oyunun log cikisi
                    size_t take = total < 4096 ? total : 4096;
                    if (take && SafeReadable(ptr, take)) {
                        std::string s(ptr, ptr + take);
                        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                        if (!s.empty()) LOG_INFO("[GAME-LOG] " + s);
                        // TANI: menu etiketleri icin bir numarali supheli, dil
                        // kodunun BOSALMASI ("langcode is us" -> "langcode is ").
                        // Bu mesaji basan CAGRI NOKTASINI yakalayip RVA'ya
                        // ceviriyoruz; sonra disassembly ile langcode'un hangi
                        // global'den okundugunu bulacagiz.
                        if (s.find("SAVEGAME MISSING") != std::string::npos) {
                            uint64_t* rsp_p = reinterpret_cast<uint64_t*>(ctx->Rsp);
                            uint64_t  ret   = (SafeReadable(rsp_p, 8)) ? *rsp_p : 0;
                            printf("[LANGCODE] \"%s\" | cagiran RVA=0x%llx%s\n", s.c_str(),
                                   static_cast<unsigned long long>(ret - g_base_addr),
                                   (s.find("is )") != std::string::npos ||
                                    s.find("is  )") != std::string::npos) ? "  <== BOS!" : "");
                            fflush(stdout);
                        }
                    }
                    ctx->Rax = static_cast<uint64_t>(cnt);
                }
                special_return_set = true;
            } else if (readable_name == "fseek") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                long off = static_cast<long>(ctx->Rsi);
                int whence = static_cast<int>(ctx->Rdx);
                int r = g_open_files.count(f) ? fseek(f, off, whence) : -1;
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                special_return_set = true;
            } else if (readable_name == "ftell") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                long r = g_open_files.count(f) ? ftell(f) : -1;
                {
                    static int s_n = 0;
                    if (s_n < 20 || r > 65536) {
                        s_n++;
                        auto it = g_open_names.find(f);
                        std::stringstream ts;
                        ts << "[VFS] ftell #" << s_n << " ["
                           << (it != g_open_names.end() ? it->second : "?") << "] -> " << r;
                        LOG_INFO(ts.str());
                    }
                }
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(r));
                special_return_set = true;
            } else if (readable_name == "rewind") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                if (g_open_files.count(f)) rewind(f);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "feof") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                ctx->Rax = g_open_files.count(f) ? static_cast<uint64_t>(feof(f)) : 1;
                special_return_set = true;
            } else if (readable_name == "fgetc") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                int c = g_open_files.count(f) ? fgetc(f) : -1;
                ctx->Rax = static_cast<uint64_t>(static_cast<int64_t>(c));
                special_return_set = true;
            } else if (readable_name == "fgets") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                // fgets(buf, n, stream): RDI=buf, RSI=n, RDX=stream
                char* buf = reinterpret_cast<char*>(ctx->Rdi);
                int n = static_cast<int>(ctx->Rsi);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdx);
                char* r = nullptr;
                if (g_open_files.count(f) && n > 0 && SafeWritable(buf, n)) r = fgets(buf, n, f);
                ctx->Rax = reinterpret_cast<uint64_t>(r);
                special_return_set = true;
            } else if (readable_name == "fflush") {
                std::lock_guard<std::mutex> vlk(g_vfs_mutex);
                FILE* f = reinterpret_cast<FILE*>(ctx->Rdi);
                if (g_open_files.count(f)) fflush(f);
                ctx->Rax = 0;
                special_return_set = true;
            }
            // ========================================================
            // GERCEK THREAD YONETIMI (scePthreadCreate / scePthreadJoin)
            // ========================================================
            // KRITIK: Oyunun main()'i tipik olarak asil oyun dongusunu ayri
            // bir thread'de calistirir:
            //   scePthreadCreate(&t, attr, GAME_ENTRY, arg, name);
            //   scePthreadJoin(t, ...);   // oyunun bitmesini bekler
            // Bizim eski stub'imiz thread'i GERCEKTEN olusturmuyor, join de
            // aninda donuyordu; bu yuzden main "oyun bitti" sanip exit ediyordu.
            // Artik gercek bir Windows thread'i olusturup entry(arg)'i SysV ABI
            // ile calistiriyoruz (VEH process-genelinde aktif oldugu icin yeni
            // thread'deki PLT cagrilari da yakalanir).
            // ========================================================
            // SYSTEM SERVICE PARAM (dil/format ayarlari)
            // ========================================================
            // sceSystemServiceParamGetInt implement edilmemisti -> generic
            // stub *value'yu DOLDURMUYORDU. Oyun PARAM_ID_LANG (1) sorup cop
            // okuyor, langcode BOS kaliyor ve sonsuz init/kaynak-yukleme
            // dongusune (m0eyes.ogg tekrar tekrar) giriyordu. ABI KytyPS5
            // libSystemService.cpp: (param_id RDI, int* value RSI).
            else if (readable_name == "sceSystemServiceParamGetInt") {
                int param_id = static_cast<int>(ctx->Rdi);
                int* value = reinterpret_cast<int*>(ctx->Rsi);
                int v = 0;
                switch (param_id) {
                    case 1:    v = 1;   break; // LANG -> ENGLISH_US
                    case 2:    v = 1;   break; // DATE_FORMAT -> DDMMYYYY
                    case 3:    v = 1;   break; // TIME_FORMAT -> 24HOUR
                    case 4:    v = 180; break; // TIME_ZONE
                    case 5:    v = 0;   break; // SUMMERTIME
                    case 7:    v = 0;   break; // GAME_PARENTAL_LEVEL -> OFF
                    case 1000: v = 1;   break; // ENTER_BUTTON_ASSIGN -> CROSS
                    default:   v = 0;   break;
                }
                if (value != nullptr && SafeWritable(value, sizeof(int))) *value = v;
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "sceSystemServiceGetStatus") {
                // (SystemServiceStatus* status RDI). Sifirla (event_num=0 =
                // olay yok). Struct ~134 byte (int + 3 bool + reserved[127]).
                uint8_t* status = reinterpret_cast<uint8_t*>(ctx->Rdi);
                if (status != nullptr && SafeWritable(status, 0x88))
                    memset(status, 0, 0x88);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "sceSystemServiceHideSplashScreen") {
                ctx->Rax = 0; // OK (no-op, KytyPS5 gibi)
                special_return_set = true;
            }
            // ========================================================
            // CONTROLLER / PAD
            // ========================================================
            // Hepsi implement edilmemisti: PadOpen 0 (gecersiz handle)
            // donuyordu, GetControllerInformation/ReadState cikti struct'larini
            // doldurmuyordu. Oyun gecersiz handle'la controller kaydini
            // yapamayip bir tablo indeksini -1 birakiyor, sonra o indeksle
            // yazip NULL'a dusuyordu (RVA 0x2e02f7). ABI KytyPS5
            // src/libs/controller.cpp'den; handle=1, controller BAGLI.
            else if (readable_name == "PadInit" ||
                     readable_name == "PadSetMotionSensorState") {
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "qsort") {
                // void qsort(void* base, size_t nmemb, size_t size,
                //            int (*compar)(const void*, const void*))
                // ONCEDEN: NID (AEJdIVZTEmo) tabloda yoktu -> isim cozulemiyor ->
                // default stub RAX=0 donuyor ve dizi HIC SIRALANMIYORDU.
                // C2 runtime sirali diziler uzerinde IKILI ARAMA yaptigi icin
                // siralanmamis dizide anahtar aramalari basarisiz oluyor
                // ("KEY NOT FOUND: music_atten/master_atten") ve lokalize
                // metinler bos kaliyor -> menu etiketleri gorunmuyor.
                // Karsilastirici GUEST kodudur: SysV ABI ile cagriliyor
                // (bkz. HostCmpBridge / GuestCmpFn).
                {
                    void*  base  = reinterpret_cast<void*>(ctx->Rdi);
                    size_t nmemb = static_cast<size_t>(ctx->Rsi);
                    size_t size  = static_cast<size_t>(ctx->Rdx);
                    auto   cmp   = reinterpret_cast<GuestCmpFn>(ctx->Rcx);
                    // Karsilastirici GUEST kodudur; ayri bir thread'de
                    // cagiriyoruz (bkz. GuestSortThread) cunku VEH handler'inin
                    // icinden cagirmak ic ice exception uretiyor.
                    //
                    // Eskiden varsayilan KAPALI idi: grafik init'inde
                    // kilitleniyordu. O kilitlenmenin gercek sebebi bulundu ve
                    // duzeltildi - EnsureKytyGraphicsInit()'te CAS'i kaybeden
                    // thread'ler init BITMEDEN donuyordu (bkz. agc.cpp). Artik
                    // kilitlenme yok, olculdu: "[QSORT] nmemb=118 -> siralandi".
                    // Atlamak zararliydi: siralanmamis tablo Astro Bot'ta cop
                    // nesne uzerinden sanal cagriya (RVA 0x4baf39) yol aciyordu,
                    // Dreaming Sarah'ta da lokalize metin aramalari basarisiz
                    // olup menu etiketlerini bosaltiyordu.
                    // Kapatmak icin: PSEMU_QSORT=0
                    static const bool s_qsort_on = [] {
                        const char* e = std::getenv("PSEMU_QSORT");
                        return e == nullptr || e[0] != '0';
                    }();
                    bool   done  = false;
                    if (s_qsort_on && base != nullptr && cmp != nullptr && size > 0 && nmemb > 1 &&
                        nmemb < (1u << 24) && SafeWritable(base, nmemb * size)) {
                        GuestSortJob job{base, nmemb, size, cmp};
                        HANDLE th = CreateThread(nullptr, 0, GuestSortThread, &job, 0, nullptr);
                        if (th != nullptr) {
                            // Timeout: takilirsa oyunu sonsuza kilitlemeyelim.
                            done = (WaitForSingleObject(th, 4000) == WAIT_OBJECT_0);
                            if (!done) TerminateThread(th, 1);
                            CloseHandle(th);
                        }
                    }
                    static std::atomic<int> s_qs{0};
                    if (s_qs.fetch_add(1, std::memory_order_relaxed) < 12) {
                        printf("[QSORT] nmemb=%zu size=%zu cmp=%p -> %s\n", nmemb, size,
                               reinterpret_cast<void*>(cmp), done ? "siralandi" : "ATLANDI");
                        fflush(stdout);
                    }
                    ctx->Rax = 0; // qsort void
                }
                special_return_set = true;
            } else if (readable_name == "strlcpy") {
                // size_t strlcpy(char* dst, const char* src, size_t size)
                // BSD: src'yi dst'ye kopyalar (en fazla size-1 karakter), DAIMA
                // NUL ile bitirir, src'nin TAM uzunlugunu dondurur.
                // ONCEDEN: NID (SfQIZcqvvms) tabloda yoktu -> isim cozulemiyor ->
                // default stub RAX=0 donuyor ve HIC KOPYALAMA yapilmiyordu, yani
                // hedef string BOS kaliyordu. Menu etiketleri gibi kopyalanan
                // metinlerin bos gorunmesinin dogrudan sebebi bu olabilir.
                {
                    char*       dst  = reinterpret_cast<char*>(ctx->Rdi);
                    const char* src  = reinterpret_cast<const char*>(ctx->Rsi);
                    size_t      size = static_cast<size_t>(ctx->Rdx);
                    size_t      slen = (src != nullptr) ? SafeStrlen(src) : 0;
                    if (dst != nullptr && src != nullptr && size > 0 &&
                        SafeWritable(dst, size)) {
                        size_t n = (slen < size - 1) ? slen : (size - 1);
                        if (n > 0 && SafeReadable(src, n)) memcpy(dst, src, n);
                        dst[n] = '\0';
                    }
                    ctx->Rax = static_cast<uint64_t>(slen);
                }
                special_return_set = true;
            } else if (readable_name == "sceAudioOutOutput") {
                // int sceAudioOutOutput(handle, ptr[, num])
                // GERCEK donanimda bu cagri ses tamponu tuketilene kadar BLOKLAR
                // ve audio thread'ini ornekleme hizina (~48 kHz) pace'ler.
                // psemu'da isim cozulemedigi icin (NID son eki #N#O, tabloda
                // #T#T vardi -> bkz. NID-PREFIX duzeltmesi) default stub RAX=0
                // donuyordu: audio thread'i HIC beklemeden serbest doniyor,
                // olculen TUM PLT cagrilarinin ~%25'ini uretip CPU'yu boguyor
                // ve ana thread'i ac birakiyordu (yukleme bitmiyor, "donma").
                // PS4/PS5 grain = 256 ornek @48 kHz -> ~5.33 ms.
                {
                    uint32_t num = static_cast<uint32_t>(ctx->Rdx);
                    if (num == 0 || num > 4096) num = 256; // makul degilse grain varsay
                    DWORD ms = static_cast<DWORD>((static_cast<uint64_t>(num) * 1000ull) / 48000ull);
                    if (ms == 0) ms = 1;
                    // TANI: bu bloklayan uyku HANGI thread'de? Oyun thread'inde
                    // ise kare basina ~14 cagri x 5.3 ms = butun kareyi yer.
                    static std::atomic<uint64_t> s_ao{0};
                    const uint64_t an = s_ao.fetch_add(1) + 1;
                    if (an <= 4 || (an % 1500ull) == 0) {
                        printf("[AUDIO] #%llu TID=%lu num=%u uyku=%lu ms\n",
                               static_cast<unsigned long long>(an), GetCurrentThreadId(), num, ms);
                        fflush(stdout);
                    }
                    Sleep(ms);
                    ctx->Rax = num; // yazilan ornek sayisi
                }
                special_return_set = true;
            } else if (readable_name == "PadOpen" || readable_name == "PadGetHandle") {
                ctx->Rax = 1; // gecerli handle (0 DEGIL - kritik)
                special_return_set = true;
            } else if (readable_name == "PadInit" || readable_name == "scePadInit") {
                ctx->Rax = static_cast<uint64_t>(Libs::Controller::PadInit());
                special_return_set = true;
            } else if (readable_name == "PadGetControllerInformation" ||
                       readable_name == "scePadGetControllerInformation") {
                // KRITIK: bu implemente degildi ve varsayilan stub'a dusuyordu,
                // yani bilgi yapisi HIC doldurulmuyordu. Oyun "bagli kumanda
                // yok" gorup PadOpen/SetMotionSensorState/GetControllerInformation
                // ucslusunu tekrar tekrar deniyor ve PadReadState'e hic gecmiyordu
                // (logda bu ucslu 4 kez ust uste tekrarliyor).
                int   handle = static_cast<int>(ctx->Rdi);
                void* info   = reinterpret_cast<void*>(ctx->Rsi);
                if (info != nullptr && SafeWritable(info, 32)) {
                    ctx->Rax = static_cast<uint64_t>(Libs::Controller::PadGetControllerInformation(
                        handle, reinterpret_cast<Libs::Controller::PadControllerInformation*>(info)));
                } else {
                    ctx->Rax = 0;
                }
                special_return_set = true;
            } else if (readable_name == "PadSetMotionSensorState" ||
                       readable_name == "scePadSetMotionSensorState") {
                ctx->Rax = static_cast<uint64_t>(Libs::Controller::PadSetMotionSensorState(
                    static_cast<int>(ctx->Rdi), ctx->Rsi != 0));
                special_return_set = true;
            } else if (readable_name == "PadReadState" || readable_name == "scePadReadState") {
                int handle = static_cast<int>(ctx->Rdi);
                void* data = reinterpret_cast<void*>(ctx->Rsi);
                // TANI: PadReadState cagriliyor ama [PAD] satiri hic basilmiyordu,
                // yani asagidaki kosul duşuyor ve Kyty'nin okumasina HIC
                // girilmiyor - girdi bu yuzden oyuna ulasmiyor. Neden dustugunu
                // tahmin etmek yerine sayfanin gercek durumunu dokelim.
                {
                    static std::atomic<int> s_diag{0};
                    if (s_diag.fetch_add(1) < 5) {
                        MEMORY_BASIC_INFORMATION mbi {};
                        SIZE_T q = data ? VirtualQuery(data, &mbi, sizeof(mbi)) : 0;
                        printf("[PAD-TANI] handle=%d data=%p vq=%llu state=0x%lx protect=0x%lx "
                               "bolge_kalan=%llu SafeWritable=%d\n",
                               handle, data, static_cast<unsigned long long>(q),
                               static_cast<unsigned long>(mbi.State),
                               static_cast<unsigned long>(mbi.Protect),
                               q ? static_cast<unsigned long long>(
                                       reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize -
                                       reinterpret_cast<uint64_t>(data))
                                 : 0ull,
                               (data && SafeWritable(data, 120)) ? 1 : 0);
                        fflush(stdout);
                    }
                }
                if (data && SafeWritable(data, 120)) {
                    ctx->Rax = Libs::Controller::PadReadState(handle, reinterpret_cast<Libs::Controller::PadData*>(data));
                    // ===== GECICI: ANA MENUYU ATLAMA =====
                    // Menu yazilari gorunmuyor ama menu MANTIGI calisiyor olabilir.
                    // Onay tusunu nabiz halinde enjekte edip oyuna girmeyi deniyoruz
                    // (CROSS, sonra CIRCLE - bolgeye gore onay tusu degisebilir).
                    // Ayrica gercek pad sorgu sayisini olcuyoruz: oyun menude pad'i
                    // hic sorgulamiyorsa enjeksiyon ise yaramaz (baska yol gerekir).
                    // PadData: buttons = offset 0 (uint32) [gpu/src/libs/padData.h].
                    // Varsayilan KAPALI; menuyu atlamak icin: PSEMU_AUTO_CONFIRM=1
                    static const bool s_auto_confirm = (std::getenv("PSEMU_AUTO_CONFIRM") != nullptr);
                    static std::atomic<uint64_t> s_pad_calls{0};
                    uint64_t pc = s_pad_calls.fetch_add(1) + 1;
                    uint32_t phase = static_cast<uint32_t>(pc % 240ull);
                    uint32_t btn = 0;
                    if (s_auto_confirm) {
                        if (phase < 20)                       btn = 0x4000u; // CROSS
                        else if (phase >= 120 && phase < 140) btn = 0x2000u; // CIRCLE
                        if (btn != 0) {
                            *reinterpret_cast<uint32_t*>(data) |= btn;
                        }
                    }
                    if (pc <= 3 || (pc % 300ull) == 0) {
                        printf("[PAD] okuma=%llu phase=%u btn=0x%04x (menu-atlama enjeksiyonu)\n",
                               static_cast<unsigned long long>(pc), phase, btn);
                        fflush(stdout);
                    }
                } else {
                    ctx->Rax = -2137915391; // PAD_ERROR_INVALID_ARG
                }
                special_return_set = true;
            } else if (readable_name == "PadGetControllerInformation") {
                // (handle RDI, info RSI). info doldurulmali; bir DualSense
                // bagliymis gibi raporla ki oyun menu/oyuna ilerlesin.
                uint8_t* info = reinterpret_cast<uint8_t*>(ctx->Rsi);
                if (info && SafeWritable(info, 0x20)) {
                    memset(info, 0, 0x20);
                    float dens = 44.86f;
                    memcpy(info + 0x00, &dens, 4);          // touch_pixel_density
                    *reinterpret_cast<uint16_t*>(info + 4) = 1920; // touch_res_x
                    *reinterpret_cast<uint16_t*>(info + 6) = 943;  // touch_res_y
                    info[8]  = 0x1e; // stick_dead_zone_left
                    info[9]  = 0x1e; // stick_dead_zone_right
                    info[10] = 0;    // connection_type
                    info[11] = 1;    // connected_count
                    info[12] = 1;    // connected = true
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "SaveDataInitialize3" ||
                     readable_name == "SaveDataSetParam" ||
                     readable_name == "SaveDataSaveIcon" ||
                     readable_name == "SaveDataCommit" ||
                     readable_name == "SaveDataPrepare" ||
                     readable_name == "SaveDataCreateTransactionResource" ||
                     readable_name == "SaveDataUmount2") {
                ctx->Rax = 0; // OK
                special_return_set = true;
            } else if (readable_name == "SaveDataDirNameSearch") {
                // (cond RDI, result RSI): result CIKTI struct'i, kayit YOK.
                // hit_num=0 kritik - dolmazsa oyun cop sayiyla iterasyona
                // girip NULL'a dusuyordu.
                uint8_t* r = reinterpret_cast<uint8_t*>(ctx->Rsi);
                if (r && SafeWritable(r, 0x40)) {
                    memset(r, 0, 0x40); // hit_num=0, dir_names=NULL, set_num=0...
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "SaveDataMount3") {
                // (mount RDI, mount_result RSI). Kayit yok -> NOT_FOUND.
                // mount_result yine de sifirlanir ki oyun cop mount noktasi
                // okumasin.
                uint8_t* mr = reinterpret_cast<uint8_t*>(ctx->Rsi);
                if (mr && SafeWritable(mr, 0x40)) {
                    memset(mr, 0, 0x40);
                }
                ctx->Rax = static_cast<uint64_t>(
                    static_cast<uint32_t>(0x809F0008)); // SAVE_DATA_ERROR_NOT_FOUND
                special_return_set = true;
            } else if (readable_name == "SaveDataDialogGetStatus") {
                // Diyalog acilmadi -> "finished"(2) veya "none"(0). Oyun
                // sonucu beklemesin diye bittigini soyluyoruz.
                ctx->Rax = 2;
                special_return_set = true;
            }
            else if (readable_name == "scePthreadMutexInit" ||
                     readable_name == "pthread_mutex_init") {
                GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rdi));
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadMutexLock" ||
                       readable_name == "pthread_mutex_lock") {
                GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rdi));
                if (m) EnterCriticalSection(&m->cs);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadMutexTrylock" ||
                       readable_name == "pthread_mutex_trylock") {
                GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rdi));
                bool got = (m != nullptr) && (TryEnterCriticalSection(&m->cs) != 0);
                ctx->Rax = got ? 0 : 16; // EBUSY
                special_return_set = true;
            } else if (readable_name == "scePthreadMutexUnlock" ||
                       readable_name == "pthread_mutex_unlock") {
                GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rdi));
                if (m) LeaveCriticalSection(&m->cs);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadMutexDestroy" ||
                       readable_name == "pthread_mutex_destroy") {
                // Tutamaci serbest birakmiyoruz: baska thread hala bekliyor
                // olabilir ve sizinti, use-after-free'den cok daha iyidir.
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadCondInit" ||
                       readable_name == "pthread_cond_init") {
                GetOrCreateCond(reinterpret_cast<uint64_t*>(ctx->Rdi));
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadCondWait" ||
                       readable_name == "pthread_cond_wait" ||
                       readable_name == "scePthreadCondTimedwait" ||
                       readable_name == "pthread_cond_timedwait") {
                // (cond, mutex[, timeout]) - mutex kilitli gelir, kilitli doner.
                GuestCond*  c = GetOrCreateCond(reinterpret_cast<uint64_t*>(ctx->Rdi));
                GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rsi));
                DWORD ms = INFINITE;
                
                if (readable_name == "scePthreadCondTimedwait") {
                    // PS5 scePthreadCondTimedwait passes usec by VALUE in RDX
                    int32_t usec = static_cast<int32_t>(ctx->Rdx);
                    if (usec < 0) {
                        // GameMaker passes -1000 here! If we set ms=0 or 1, it returns ETIMEDOUT too fast 
                        // and breaks the async loader (black screen). 
                        // Give it a large timeout (equivalent to the old uint32 cast).
                        ms = INFINITE - 1; 
                    } else {
                        ms = static_cast<DWORD>(usec / 1000);
                        if (ms == 0) ms = 1; // don't return immediately
                    }
                } else if (readable_name == "pthread_cond_timedwait") {
                    // POSIX passes struct timespec* abstime in RDX
                    uint64_t ptr = ctx->Rdx;
                    if (ptr && SafeReadable((void*)ptr, 16)) {
                        int64_t tv_sec = *reinterpret_cast<int64_t*>(ptr);
                        int64_t tv_nsec = *reinterpret_cast<int64_t*>(ptr + 8);
                        
                        // Convert abstime to relative ms (simplified, usually we'd get current time)
                        // For now just wait 1ms so we don't hang, since calculating precise relative time
                        // requires knowing which clock (CLOCK_REALTIME) was used.
                        ms = 1; 
                    } else {
                        ms = 0;
                    }
                }

                BOOL ok = TRUE;
                if (c && m) {
                    ok = SleepConditionVariableCS(&c->cv, &m->cs, ms);
                } else {
                    // Tutamac kurulamadi: en azindan bekle, MESGUL DONGU olma.
                    Sleep(ms == INFINITE ? 1u : (ms ? ms : 1u));
                    ok = FALSE;
                }
                
                // Zaman asimi -> ETIMEDOUT(60).
                ctx->Rax = ok ? 0 : 60;
                special_return_set = true;
            } else if (readable_name == "scePthreadCondSignal" ||
                       readable_name == "pthread_cond_signal") {
                GuestCond* c = GetOrCreateCond(reinterpret_cast<uint64_t*>(ctx->Rdi));
                if (c) WakeConditionVariable(&c->cv);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadCondBroadcast" ||
                       readable_name == "pthread_cond_broadcast") {
                GuestCond* c = GetOrCreateCond(reinterpret_cast<uint64_t*>(ctx->Rdi));
                if (c) WakeAllConditionVariable(&c->cv);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "scePthreadCondDestroy" ||
                       readable_name == "pthread_cond_destroy") {
                ctx->Rax = 0;
                special_return_set = true;
            }
            else if (readable_name == "scePthreadCreate") {
                // scePthreadCreate(ScePthread* thread, attr, entry, arg, name)
                //   RDI=thread_out, RSI=attr, RDX=entry, RCX=arg, R8=name
                uint64_t entry = ctx->Rdx;
                uint64_t arg   = ctx->Rcx;
                void** thread_out = reinterpret_cast<void**>(ctx->Rdi);

                void* stub = BuildSysVTramp(entry, arg, 0, 0); // RDI=arg
                HANDLE h = nullptr;
                if (stub) {
                    // Genis stack (64MB reserve) ver: mesru derin ozyineleme
                    // basarsin, runaway ozyinelemede de watchdog ornek alacak
                    // zamani bulsun. STACK_SIZE_PARAM_IS_A_RESERVATION.
                    h = CreateThread(NULL, 64ull * 1024 * 1024, GamePthreadProc, stub,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
                }
                if (h) {
                    // Watchdog icin BAGIMSIZ bir handle kopyasi al; oyuna verilen
                    // handle join'de CloseHandle ile kapatilinca watchdog'unki
                    // gecerli kalsin (yoksa kapali handle uzerinde islem olurdu).
                    HANDLE dup = nullptr;
                    if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(),
                                        &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                        g_worker_thread = dup;
                    } else {
                        g_worker_thread = h;
                    }
                    g_last_activity = GetTickCount64();
                }
                if (thread_out != nullptr && SafeWritable(thread_out, sizeof(void*))) {
                    *thread_out = reinterpret_cast<void*>(h); // ScePthread = Windows HANDLE
                }
                ctx->Rax = h ? 0 : static_cast<uint64_t>(-1LL);
                special_return_set = true;

                std::stringstream pc_ss;
                pc_ss << "[THREAD-HLE] scePthreadCreate: entry=0x" << std::hex << entry
                      << " (RVA 0x" << (IsInModuleRange(entry) ? entry - g_base_addr : 0) << ")"
                      << " arg=0x" << arg << " -> HANDLE=0x" << reinterpret_cast<uint64_t>(h) << std::dec;
                LOG_INFO(pc_ss.str());
            } else if (readable_name == "scePthreadJoin") {
                // scePthreadJoin(ScePthread thread, void** value): RDI=thread(HANDLE)
                HANDLE h = reinterpret_cast<HANDLE>(ctx->Rdi);
                if (h != nullptr) {
                    LOG_INFO("[THREAD-HLE] scePthreadJoin: worker thread bekleniyor (INFINITE)...");
                    WaitForSingleObject(h, INFINITE);
                    CloseHandle(h);
                    LOG_INFO("[THREAD-HLE] scePthreadJoin: worker thread bitti.");
                }
                ctx->Rax = 0;
                special_return_set = true;
            }
            // ========================================================
            // TEMIZ CIKIS (exit / _Exit) - ud2 tuzagina dusmeyi onler
            // ========================================================
            // exit() donmemeli; eski stub RET simule edince cagri sonrasi
            // derleyicinin koydugu "unreachable" tuzagina (ud2 -> illegal
            // instruction) dusuyorduk. Artik process'i temiz sonlandiriyoruz.
            else if (readable_name == "exit" || readable_name == "_Exit") {
                int code = static_cast<int>(ctx->Rdi);
                std::stringstream ex_ss;
                ex_ss << "[EXIT-HLE] Oyun exit(" << code << ") cagirdi, process temiz sonlandiriliyor.";
                LOG_INFO(ex_ss.str());
                fflush(stdout);
                ExitProcess(static_cast<UINT>(code));
            }
            // ========================================================
            // GRAFIK (GNM/Gen5) BASLATMA - GEÃ‡ICI STUB
            // ========================================================
            // Oyun sistem servislerini gecip GPU init'e ulasti. Bu fonksiyonlar
            // gercek GPU register-default tablolarina pointer donduruyor (KytyPS5
            // agc.cpp: get_public_register_defaults). Tam GNM emulasyonu devasa
            // bir is; simdilik SIFIRLANMIS bir buffer donduruyoruz. Caller
            // [ptr+0x38] gibi offsetlerden 0 okuyup "yapacak is yok" ile devam
            // ediyor - bu bizi bir sonraki grafik duvarina kadar ilerletir.
            else if (readable_name == "GraphicsGetRegisterDefaults2" ||
                     readable_name == "GraphicsGetRegisterDefaults2Internal") {
                static void* s_reg_defaults = nullptr;
                if (s_reg_defaults == nullptr) {
                    // VirtualAlloc MEM_COMMIT sifirlanmis bellek verir; kalici birak.
                    s_reg_defaults = VirtualAlloc(nullptr, 0x10000,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(s_reg_defaults);
                special_return_set = true;
                std::stringstream gd_ss;
                gd_ss << "[GFX-HLE] " << readable_name << " -> sifir buffer 0x"
                      << std::hex << reinterpret_cast<uint64_t>(s_reg_defaults) << std::dec;
                LOG_INFO(gd_ss.str());
            } else if (readable_name == "GraphicsInit") {
                // GraphicsInit(uint32_t* state, uint32_t ver): RDI=state, RSI=ver
                uint32_t* state = reinterpret_cast<uint32_t*>(ctx->Rdi);
                uint32_t ver = static_cast<uint32_t>(ctx->Rsi);
                if (state != nullptr && SafeWritable(state, 8)) {
                    state[0] = ver;
                    state[1] = 0; // GRAPHICS_INIT_NO_FEATURE_STATE
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "GraphicsCreateShader") {
                // int GraphicsCreateShader(Shader** dst, void* header, const void* code)
                //   RDI = dst (CIKTI!), RSI = header, RDX = code
                // KytyPS5 (src/libs/agc.cpp) referansiyla dogrulandi.
                //
                // Shader header'i (yuklenen ELF icinde) pointer alanlarini
                // KENDI KONUMUNA GORELI saklar; bu fonksiyonun gorevi onlari
                // mutlak adrese cevirip (m += &m) *dst'ye header'i yazmaktir.
                // Eskiden RAX'ta sahte bir nesne donduruyorduk; oyun *dst'yi
                // (sifir) okuyup h->cx_registers (+0x18) NULL cikinca
                // RVA 0x29516'da cokÃ¼yordu.
                //
                // Shader struct offsetleri (KytyPS5 shader.h):
                //   0x08 user_data  0x10 code         0x18 cx_registers
                //   0x20 sh_registers 0x28 specials   0x30 input_semantics
                //   0x38 output_semantics
                void** dst   = reinterpret_cast<void**>(ctx->Rdi);
                uint8_t* h   = reinterpret_cast<uint8_t*>(ctx->Rsi);
                uint64_t code = ctx->Rdx;

                if (h != nullptr && SafeReadable(h, 0x48)) {
                    // Kendi konumuna goreli pointer'i mutlaka cevir: m += &m
                    auto fixup = [&](size_t off) {
                        uint64_t* p = reinterpret_cast<uint64_t*>(h + off);
                        if (*p != 0) *p += reinterpret_cast<uint64_t>(p);
                    };
                    fixup(0x08); // user_data
                    fixup(0x18); // cx_registers
                    fixup(0x20); // sh_registers
                    fixup(0x28); // specials
                    fixup(0x30); // input_semantics
                    fixup(0x38); // output_semantics

                    // user_data alt alanlari (ShaderUserData):
                    //   0x00 direct_resource_offset, 0x08..0x20 sharp_resource_offset[4]
                    uint64_t ud = *reinterpret_cast<uint64_t*>(h + 0x08);
                    if (ud != 0 && SafeReadable(reinterpret_cast<void*>(ud), 0x28)) {
                        uint8_t* u = reinterpret_cast<uint8_t*>(ud);
                        for (size_t o = 0; o <= 0x20; o += 8) {
                            uint64_t* p = reinterpret_cast<uint64_t*>(u + o);
                            if (*p != 0) *p += reinterpret_cast<uint64_t>(p);
                        }
                    }

                    *reinterpret_cast<uint64_t*>(h + 0x10) = code; // h->code = code
                }

                // Asil sonuc: *dst = header
                if (dst != nullptr && SafeReadable(dst, 8)) {
                    *dst = h;
                }
                ctx->Rax = 0; // basari
                special_return_set = true;

                std::stringstream sh;
                sh << "[GFX-HLE] GraphicsCreateShader: header=0x" << std::hex
                   << reinterpret_cast<uint64_t>(h) << " code=0x" << code
                   << " -> *dst yazildi" << std::dec;
                LOG_INFO(sh.str());
            }
            // ========================================================
            // GNM KOMUT TAMPONU YAZICILARI
            // ========================================================
            // Bunlar oyunun CommandBuffer'ina PM4 paketi yazip YAZDIKLARI YERIN
            // ADRESINI dondurur. 0 donmek NULL zincirine yol aciyordu:
            //   cmd=NULL -> GetDataPacketPayloadAddress: *addr = cmd+2 = 8
            //            -> oyun [8]'e yazinca RVA 0x5f0f'te cokuyordu.
            // PM4 icerigini biz yorumlamiyoruz; onemli olan gecerli pointer ve
            // imlecin dogru ilerlemesi (KytyPS5 agc.cpp: AllocateDW).
            else if (readable_name == "GraphicsDcbSetCxRegistersIndirect") {
                // IT_SET_CONTEXT_REG_INDIRECT = 0x9F, R=0
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 5);
                if (cmd) {
                    cmd[0] = Pm4Header(5, 0x9F);
                    uint64_t regs = static_cast<uint64_t>(ctx->Rsi);
                    cmd[1] = static_cast<uint32_t>(regs & 0xfffffffcu);
                    cmd[2] = static_cast<uint32_t>(regs >> 32u);
                    cmd[3] = 0x80000000u;
                    cmd[4] = static_cast<uint32_t>(ctx->Rdx) & 0x3fffu;
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "GraphicsDcbSetShRegistersIndirect") {
                // IT_SET_SH_REG_INDIRECT = 0x63, R=0
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 5);
                if (cmd) {
                    cmd[0] = Pm4Header(5, 0x63);
                    uint64_t regs = static_cast<uint64_t>(ctx->Rsi);
                    cmd[1] = static_cast<uint32_t>(regs & 0xfffffffcu);
                    cmd[2] = static_cast<uint32_t>(regs >> 32u);
                    cmd[3] = 0x80000000u;
                    cmd[4] = static_cast<uint32_t>(ctx->Rdx) & 0x3fffu;
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "GraphicsDcbSetUcRegistersIndirect") {
                // IT_SET_UCONFIG_REG_INDIRECT = 0x64, R=0
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 5);
                if (cmd) {
                    cmd[0] = Pm4Header(5, 0x64);
                    uint64_t regs = static_cast<uint64_t>(ctx->Rsi);
                    cmd[1] = static_cast<uint32_t>(regs & 0xfffffffcu);
                    cmd[2] = static_cast<uint32_t>(regs >> 32u);
                    cmd[3] = 0x80000000u;
                    cmd[4] = static_cast<uint32_t>(ctx->Rdx) & 0x3fffu;
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "sceAgcDcbDrawIndexOffset") {
                // IT_DRAW_INDEX_OFFSET_2 = 0x35, R=0
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 5);
                if (cmd) {
                    cmd[0] = Pm4Header(5, 0x35);
                    uint32_t count = static_cast<uint32_t>(ctx->Rdx);
                    cmd[1] = count == 0 ? 1u : count;
                    cmd[2] = static_cast<uint32_t>(ctx->Rsi);
                    cmd[3] = count;
                    uint64_t mod = static_cast<uint64_t>(ctx->Rcx);
                    cmd[4] = (mod & (1ull << 32u)) ? 0u : ((static_cast<uint32_t>(mod) >> 3u) & 0x20u);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "sceAgcDcbSetIndexBuffer") {
                // IT_INDEX_BASE = 0x26, R=0, 3 DWORDs
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 3);
                if (cmd) {
                    cmd[0] = Pm4Header(3, 0x26);
                    uint64_t addr = static_cast<uint64_t>(ctx->Rsi);
                    cmd[1] = static_cast<uint32_t>(addr & 0xffffffffu);
                    cmd[2] = static_cast<uint32_t>(addr >> 32u);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "sceAgcDcbSetIndexSize") {
                // IT_SET_UCONFIG_REG_INDEX = 0x7A, VGT_INDEX_TYPE=0x243, 3 DWORDs
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 3);
                if (cmd) {
                    cmd[0] = Pm4Header(3, 0x7A);
                    cmd[1] = 0x20000000u | 0x243u;
                    uint8_t agc_sz = static_cast<uint8_t>(ctx->Rsi);
                    // Agc enum: 0=8bit, 1=16bit, 2=32bit (or 4=32bit)
                    // Kyty expects: 0=16bit, 1=32bit, 2=8bit
                    uint8_t kyty_sz = (agc_sz == 1) ? 0 : ((agc_sz == 2 || agc_sz == 4) ? 1 : 2);
                    uint8_t cache_policy = static_cast<uint8_t>(ctx->Rdx);
                    cmd[2] = 0x400u | (kyty_sz & 0x3u) | ((cache_policy & 0x3u) << 6u);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "sceAgcDcbSetFlip") {
                // KYTY_PM4(6, IT_NOP=0x10, R_FLIP=0x17)
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 6);
                if (cmd) {
                    // 0xC0000000 | ((6-2)<<16) | (0x10<<8) | (0x17<<2)
                    cmd[0] = 0xC004105Cu;
                    cmd[1] = static_cast<uint32_t>(ctx->Rsi);
                    cmd[2] = static_cast<uint32_t>(ctx->Rdx);
                    cmd[3] = static_cast<uint32_t>(ctx->Rcx);
                    uint64_t arg = static_cast<uint64_t>(ctx->R8);
                    cmd[4] = static_cast<uint32_t>(arg & 0xffffffffu);
                    cmd[5] = static_cast<uint32_t>(arg >> 32u);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "GraphicsCbSetShRegisterRangeDirect") {
                // (CommandBuffer* buf, uint32_t offset, const uint32_t* values,
                //  uint32_t num_values) -> uint32_t*
                uint32_t  offset     = static_cast<uint32_t>(ctx->Rsi);
                uint32_t* values     = reinterpret_cast<uint32_t*>(ctx->Rdx);
                uint32_t  num_values = static_cast<uint32_t>(ctx->Rcx);

                uint32_t* cmd = CbAllocateDW(ctx->Rdi, num_values + 2);
                if (cmd) {
                    cmd[0] = Pm4Header(num_values + 2, kPm4_IT_SET_SH_REG);
                    cmd[1] = offset & 0xffffu;
                    if (values && num_values &&
                        SafeReadable(values, static_cast<size_t>(num_values) * 4)) {
                        memcpy(cmd + 2, values, static_cast<size_t>(num_values) * 4);
                    } else {
                        memset(cmd + 2, 0, static_cast<size_t>(num_values) * 4);
                    }
                }
                ctx->Rax = reinterpret_cast<uint64_t>(cmd);
                special_return_set = true;
            } else if (readable_name == "GraphicsGetDataPacketPayloadAddress") {
                // int (uint32_t** addr, uint32_t* cmd, int type)
                //   type != 0 -> *addr = cmd + 2
                //   type == 0 -> *addr = (~cmd[0] & 0x3fff0000) ? cmd+1 : nullptr
                uint32_t** addr = reinterpret_cast<uint32_t**>(ctx->Rdi);
                uint32_t*  cmd  = reinterpret_cast<uint32_t*>(ctx->Rsi);
                int        type = static_cast<int>(ctx->Rdx);

                if (addr != nullptr && SafeReadable(addr, 8)) {
                    if (cmd == nullptr) {
                        // Kaynak komut yoksa 8 gibi sahte bir adres uretmek
                        // yerine acikca NULL birak (oyun kontrol edebilsin).
                        *addr = nullptr;
                    } else if (type != 0) {
                        *addr = cmd + 2;
                    } else {
                        uint32_t cmd_id = SafeReadable(cmd, 4) ? cmd[0] : 0;
                        *addr = ((~cmd_id & 0x3fff0000u) != 0) ? (cmd + 1) : nullptr;
                    }
                }
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "GraphicsCreatePrimState") {
                // (ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                //  const Shader* hs, const Shader* gs, uint32_t prim_type)
                // ShaderRegister = { uint32_t offset; uint32_t value; }
                // gs->specials (Shader+0x28) icinden register degerlerini kopyalar.
                uint32_t* cx = reinterpret_cast<uint32_t*>(ctx->Rdi);
                uint32_t* uc = reinterpret_cast<uint32_t*>(ctx->Rsi);
                uint8_t*  gs = reinterpret_cast<uint8_t*>(ctx->Rcx);
                uint32_t  prim_type = static_cast<uint32_t>(ctx->R8);

                // ShaderSpecialRegs: 0x00 ge_cntl, 0x08 ge_user_vgpr_en, ...
                // (her biri 8 byte'lik ShaderRegister)
                uint8_t* specials = nullptr;
                if (gs && SafeReadable(gs + 0x28, 8)) {
                    specials = *reinterpret_cast<uint8_t**>(gs + 0x28);
                    if (specials && !SafeReadable(specials, 0x20)) specials = nullptr;
                }

                if (cx && SafeReadable(cx, 16)) {
                    memset(cx, 0, 16); // 2 adet ShaderRegister
                }
                if (uc && SafeReadable(uc, 24)) {
                    memset(uc, 0, 24); // 3 adet ShaderRegister
                    if (specials) {
                        memcpy(uc + 0, specials + 0x00, 8); // ge_cntl
                        memcpy(uc + 2, specials + 0x08, 8); // ge_user_vgpr_en
                    }
                    uc[4] = 0x2242;    // VGT_PRIMITIVE_TYPE offset
                    uc[5] = prim_type;
                }
                ctx->Rax = 0;
                special_return_set = true;
            }
            // ========================================================
            // AGC (GPU / HLE render) â€” sceAgc* + Graphics* yuzeyi
            // ========================================================
            // Oyun render'i AGC ile yapiyor. Agc::Dispatch bu fonksiyonlari
            // sahiplenir (flip'i Video'ya baglar, render-state'i yakalar);
            // AGC disi isimlerde false donup zincirin devamina birakir.
            else if (Agc::Dispatch(func_name, readable_name, ctx)) {
                special_return_set = true;
            }
            // ========================================================
            // VIDEO OUT (ekran sunumu)
            // ========================================================
            // Oyun kendi framebuffer'larini RegisterBuffers2 ile bize verir,
            // SubmitFlip ile "ekrana bas" der. Win32 penceresine blit ediyoruz.
            else if (readable_name == "sceVideoOutOpen") {
                int user_id  = static_cast<int>(ctx->Rdi);
                int bus_type = static_cast<int>(ctx->Rsi);
                int index    = static_cast<int>(ctx->Rdx);
                const void* param = reinterpret_cast<const void*>(ctx->Rcx);
                ctx->Rax = Libs::VideoOut::VideoOutOpen(user_id, bus_type, index, param);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutAddFlipEvent") {
                Libs::LibKernel::EventQueue::KernelEqueue eq = reinterpret_cast<Libs::LibKernel::EventQueue::KernelEqueue>(ctx->Rdi);
                int handle = static_cast<int>(ctx->Rsi);
                void* udata = reinterpret_cast<void*>(ctx->Rdx);
                ctx->Rax = Libs::VideoOut::VideoOutAddFlipEvent(eq, handle, udata);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutAddVblankEvent") {
                Libs::LibKernel::EventQueue::KernelEqueue eq = reinterpret_cast<Libs::LibKernel::EventQueue::KernelEqueue>(ctx->Rdi);
                int handle = static_cast<int>(ctx->Rsi);
                void* udata = reinterpret_cast<void*>(ctx->Rdx);
                ctx->Rax = Libs::VideoOut::VideoOutAddVblankEvent(eq, handle, udata);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutSetBufferAttribute2") {
                uint32_t* attr = reinterpret_cast<uint32_t*>(ctx->Rdi);
                uint64_t pixel_format = ctx->Rsi;
                uint32_t tiling = static_cast<uint32_t>(ctx->Rdx);
                uint32_t width  = static_cast<uint32_t>(ctx->Rcx);
                uint32_t height = static_cast<uint32_t>(ctx->R8);
                if (attr && SafeReadable(attr, 64)) {
                    memset(attr, 0, 64);
                    attr[1] = tiling;          // tiling_mode
                    attr[2] = 0;               // aspect_ratio
                    attr[3] = width;           // width
                    attr[4] = height;          // height
                    attr[5] = width;           // pitch_in_pixel
                    *reinterpret_cast<uint64_t*>(attr + 8) = pixel_format; // +0x20
                }
                Video::SetAttribute(width, height, width, pixel_format, tiling);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "sceVideoOutRegisterBuffers2") {
                int handle    = static_cast<int>(ctx->Rdi);
                int set_index = static_cast<int>(ctx->Rsi);
                int start     = static_cast<int>(ctx->Rdx);
                const Libs::VideoOut::VideoOutBuffers* bufs = reinterpret_cast<const Libs::VideoOut::VideoOutBuffers*>(ctx->Rcx);
                int num       = static_cast<int>(ctx->R8);
                const Libs::VideoOut::VideoOutBufferAttribute2* attr = reinterpret_cast<const Libs::VideoOut::VideoOutBufferAttribute2*>(ctx->R9);
                ctx->Rax = Libs::VideoOut::VideoOutRegisterBuffers2(handle, set_index, start, bufs, num, attr, 0, nullptr);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutSubmitFlip") {
                int handle    = static_cast<int>(ctx->Rdi);
                int index     = static_cast<int>(ctx->Rsi);
                int flip_mode = static_cast<int>(ctx->Rdx);
                int64_t arg   = static_cast<int64_t>(ctx->Rcx);
                Video::Flip(index);
                ctx->Rax = Libs::VideoOut::VideoOutSubmitFlip(handle, index, flip_mode, arg);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutGetFlipStatus") {
                int handle = static_cast<int>(ctx->Rdi);
                Libs::VideoOut::VideoOutFlipStatus* status = reinterpret_cast<Libs::VideoOut::VideoOutFlipStatus*>(ctx->Rsi);
                ctx->Rax = Libs::VideoOut::VideoOutGetFlipStatus(handle, status);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutIsFlipPending") {
                int handle = static_cast<int>(ctx->Rdi);
                ctx->Rax = Libs::VideoOut::VideoOutIsFlipPending(handle);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutWaitVblank") {
                int handle = static_cast<int>(ctx->Rdi);
                ctx->Rax = Libs::VideoOut::VideoOutWaitVblank(handle);
                special_return_set = true;
            } else if (readable_name == "sceVideoOutIsOutputSupported") {
                ctx->Rax = 1; // destekleniyor
                special_return_set = true;
            }
            // ========================================================
            // OYUNUN KENDI LOG CIKTISI (vsnprintf/vfprintf/fputs/puts)
            // ========================================================
            // Oyun kendi log mesajlarini bu fonksiyonlarla uretiyor. Eskiden
            // RAX=0 donup mesaji YUTUYORDUK; artik gercekten formatlayip
            // [GAME-LOG] olarak gosteriyoruz - oyun bize ne yaptigini soyler.
            else if (readable_name == "vsnprintf") {
                // vsnprintf(buf, size, fmt, va): RDI=buf, RSI=size, RDX=fmt, RCX=va
                char* buf = reinterpret_cast<char*>(ctx->Rdi);
                size_t size = static_cast<size_t>(ctx->Rsi);
                const char* fmt = reinterpret_cast<const char*>(ctx->Rdx);
                std::string s = FormatSysVPrintf(fmt, reinterpret_cast<uint8_t*>(ctx->Rcx));
                bool wrote = false;
                if (size > 0 && SafeWritable(buf, size)) {
                    size_t n = s.size() < (size - 1) ? s.size() : (size - 1);
                    memcpy(buf, s.data(), n);
                    buf[n] = 0;
                    wrote = true;
                }
                // Mesaji BURADA logla: buffer'a yazma veya sonraki fputs basarisiz
                // olsa bile oyunun ne demek istedigini kesin goruruz.
                // TANI (menu etiketleri): dil kodunun BOSALMASINI izliyoruz.
                // Cagiran RVA'yi ve va-alanindaki ilk pointer'lari dokuyoruz;
                // boylece langcode'un hangi bellekten okundugunu bulup oraya
                // yazan kodu yakalayacagiz.
                if (s.find("SAVEGAME MISSING") != std::string::npos) {
                    uint64_t* rsp_p = reinterpret_cast<uint64_t*>(ctx->Rsp);
                    uint64_t  ret   = SafeReadable(rsp_p, 8) ? *rsp_p : 0;
                    std::stringstream lc;
                    lc << "[LANGCODE] \"" << s << "\" | cagiran RVA=0x" << std::hex
                       << (ret - g_base_addr) << " | va=0x" << ctx->Rcx;
                    const uint64_t* va = reinterpret_cast<const uint64_t*>(ctx->Rcx);
                    if (va != nullptr && SafeReadable(va, 48)) {
                        lc << " va[0..5]=";
                        for (int q = 0; q < 6; q++) lc << "0x" << va[q] << " ";
                    }
                    LOG_INFO(lc.str());
                }
                if (!s.empty()) {
                    LOG_INFO("[GAME-LOG] " + s + (wrote ? "" : "   <-- [UYARI: buffer'a yazilamadi]"));
                } else {
                    // Bos sonuc = formatlama basarisiz; ham format string'i goster
                    std::string raw = SafeReadCString(fmt);
                    std::stringstream d;
                    d << "[GAME-LOG-DEBUG] format bos dondu. fmt=0x" << std::hex << ctx->Rdx
                      << " okunabilir=" << (SafeReadable(fmt, 1) ? "EVET" : "HAYIR")
                      << " ham=\"" << raw << "\"";
                    LOG_INFO(d.str());
                }
                ctx->Rax = static_cast<uint64_t>(s.size());
                special_return_set = true;
            } else if (readable_name == "vfprintf") {
                // vfprintf(stream, fmt, va): RDI=stream, RSI=fmt, RDX=va
                const char* fmt = reinterpret_cast<const char*>(ctx->Rsi);
                std::string s = FormatSysVPrintf(fmt, reinterpret_cast<uint8_t*>(ctx->Rdx));
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) LOG_INFO("[GAME-LOG] " + s);
                ctx->Rax = static_cast<uint64_t>(s.size());
                special_return_set = true;
            } else if (readable_name == "fputs" || readable_name == "puts") {
                // fputs(str, stream) / puts(str): her ikisinde de RDI=str
                std::string s = SafeReadCString(reinterpret_cast<const char*>(ctx->Rdi));
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) LOG_INFO("[GAME-LOG] " + s);
                ctx->Rax = 0;
                special_return_set = true;
            } else if (readable_name == "printf") {
                // printf(fmt, ...): RDI=fmt, degisken argumanlar RSI..R9 + stack
                std::string s = FormatVariadicFromCtx(
                    reinterpret_cast<const char*>(ctx->Rdi), ctx, 1);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) LOG_INFO("[GAME-LOG] " + s);
                ctx->Rax = static_cast<uint64_t>(s.size());
                special_return_set = true;
            } else if (readable_name == "fprintf") {
                // fprintf(stream, fmt, ...): RDI=stream, RSI=fmt
                std::string s = FormatVariadicFromCtx(
                    reinterpret_cast<const char*>(ctx->Rsi), ctx, 2);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) LOG_INFO("[GAME-LOG] " + s);
                ctx->Rax = static_cast<uint64_t>(s.size());
                special_return_set = true;
            } else if (readable_name == "sprintf") {
                // sprintf(buf, fmt, ...): RDI=buf, RSI=fmt (2 adet sabit GP arg)
                // Bu NO-OP oldugu icin oyun dosya yollarini kuramiyor,
                // fopen("/app0/") basarisiz olup exit(1) ediyordu.
                char* buf = reinterpret_cast<char*>(ctx->Rdi);
                std::string s = FormatVariadicFromCtx(
                    reinterpret_cast<const char*>(ctx->Rsi), ctx, 2);
                if (SafeWritable(buf, s.size() + 1)) {
                    memcpy(buf, s.c_str(), s.size() + 1);
                }
                ctx->Rax = static_cast<uint64_t>(s.size());
                special_return_set = true;
            } else if (readable_name == "snprintf") {
                // snprintf(buf, size, fmt, ...): RDI=buf, RSI=size, RDX=fmt (3 sabit)
                char* buf = reinterpret_cast<char*>(ctx->Rdi);
                size_t size = static_cast<size_t>(ctx->Rsi);
                std::string s = FormatVariadicFromCtx(
                    reinterpret_cast<const char*>(ctx->Rdx), ctx, 3);
                if (size > 0 && SafeWritable(buf, size)) {
                    size_t n = s.size() < (size - 1) ? s.size() : (size - 1);
                    memcpy(buf, s.data(), n);
                    buf[n] = 0;
                }
                ctx->Rax = static_cast<uint64_t>(s.size());
                special_return_set = true;
            } else if (readable_name == "wmemchr" || readable_name == "NID_fnUEjBCNRVU") {
                // wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n)
                // PS5/FreeBSD'de wchar_t = 4 BYTE; n = ELEMAN sayisi. Onceki hook
                // byte-tabanliydi (count byte arayip byte karsilastiriyordu) ->
                // '|' ayraci count/4'ten ilerideyse BULUNAMIYOR, GameMaker'in
                // parse dongusu spin ediyor (donma) + string'ler bos kaliyor
                // (gorunmez butonlar).
                const uint32_t* p = reinterpret_cast<const uint32_t*>(ctx->Rdi);
                uint32_t        ch    = static_cast<uint32_t>(ctx->Rsi);
                size_t          count = static_cast<size_t>(ctx->Rdx);
                // DOGRULAMA: ilk birkac cagride kaynak baytlarini dok (wchar_t
                // boyutunu gozle teyit: "b\0\0\0g..." = 4B, "b\0g\0" = 2B, "bg" = 1B).
                {
                    static int s_dbg = 0;
                    if (s_dbg < 3 && p != nullptr && SafeReadable(p, 16)) {
                        s_dbg++;
                        const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
                        printf("[WMEMCHR] ptr=%p c=0x%x n=%zu bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                               (void*)p, ch, count, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
                        fflush(stdout);
                    }
                }
                const uint32_t* result = nullptr;
                if (p != nullptr && count > 0 && SafeReadable(p, count * sizeof(uint32_t))) {
                    for (size_t i = 0; i < count; i++) {
                        if (p[i] == ch) { result = &p[i]; break; }
                    }
                }
                ctx->Rax = reinterpret_cast<uint64_t>(result);
                special_return_set = true;
            } else if (readable_name == "wmemmove" || readable_name == "NID_Noj9PsJrsa8") {
                // wchar_t* wmemmove(wchar_t* d, const wchar_t* s, size_t n)
                // n = ELEMAN sayisi; kopyalanacak = n * 4 byte. Onceki hook n byte
                // kopyaliyordu -> genis karakterleri kirpiyor, string'ler bozuluyordu.
                void*       dest  = reinterpret_cast<void*>(ctx->Rdi);
                const void* src   = reinterpret_cast<const void*>(ctx->Rsi);
                size_t      count = static_cast<size_t>(ctx->Rdx);
                size_t      bytes = count * sizeof(uint32_t);
                if (dest != nullptr && src != nullptr && count > 0 &&
                    SafeReadable(src, bytes) && SafeWritable(dest, bytes)) {
                    memmove(dest, src, bytes);
                }
                ctx->Rax = reinterpret_cast<uint64_t>(dest);
                special_return_set = true;
            }

            // ========================================================
            // PLT#8 OZEL YAKALAMA (sceKernelGetProcessParam)
            // ========================================================
            if (plt_index == 8) {
                g_plt8_param_ptr  = ctx->R8;
                g_plt8_param_size = ctx->R9;
                
                std::stringstream cap_ss;
                cap_ss << "[!!!] PLT#8 YAKALANDI! ProcessParam ptr=0x" 
                       << std::hex << g_plt8_param_ptr 
                       << " size=0x" << g_plt8_param_size << std::dec;
                LOG_INFO(cap_ss.str());

                // PLT#8 register derin analiz: base_addr araligi kontrolu
                uint64_t reg_vals[] = { ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx->Rcx, ctx->R8, ctx->R9 };
                const char* reg_nm[] = { "RDI", "RSI", "RDX", "RCX", "R8", "R9" };
                for (int ri = 0; ri < 6; ri++) {
                    if (reg_vals[ri] >= g_base_addr && reg_vals[ri] < (g_base_addr + g_text_size)) {
                        std::stringstream ra_ss;
                        ra_ss << "[PLT#8-ANALIZ] " << reg_nm[ri] << "=0x" << std::hex << reg_vals[ri]
                              << " -> TEXT SEGMENT ICINDE! (RVA: 0x" << (reg_vals[ri] - g_base_addr) << ")";
                        LOG_INFO(ra_ss.str());
                    }
                }
            }
            
            // Function pointer avcisi (diger PLT'ler icin)
            if (g_game_thread_entry == 0) {
                uint64_t candidates[] = { ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx->Rcx, ctx->R8, ctx->R9 };
                const char* reg_names[] = { "RDI", "RSI", "RDX", "RCX", "R8", "R9" };
                for (int i = 0; i < 6; i++) {
                    if (IsInTextSegment(candidates[i]) && candidates[i] != 0) {
                        g_game_thread_entry = candidates[i];
                        std::stringstream gss;
                        gss << "[!!!] GAME THREAD ENTRY YAKALANDI! PLT#" << plt_index 
                            << " register " << reg_names[i] 
                            << " -> 0x" << std::hex << g_game_thread_entry << std::dec;
                        LOG_INFO(gss.str());
                        break;
                    }
                }
            }
            
                // 1. Stack'in tepesindeki donus adresini oku
                uint64_t* rsp_ptr = reinterpret_cast<uint64_t*>(ctx->Rsp);
                uint64_t ret_addr = (SafeReadable(rsp_ptr, sizeof(uint64_t))) ? *rsp_ptr : 0;

                // Guvenlik kontrolu: bu adres gercekten oyun modulunun kendi bellek
                // blogu icinde mi? Degilse, bu aslinda bir "call" ile buraya gelinmedigini
                // (ornegin tail-jmp veya call derinligi desenkronize oldugunu) gosterir;
                // kor bir sekilde oraya sicramak (ozellikle loader.exe'nin kendi native
                // kod/veri bolgesine) izlemesi imkansiz ikincil cokmelere yol acar.
                if (!IsInModuleRange(ret_addr)) {
                    std::stringstream bad_ss;
                    bad_ss << "[-] HATA: PLT-HOOK RET adresi (0x" << std::hex << ret_addr
                           << ") oyun modulunun disinda! (base=0x" << g_base_addr
                           << " size=0x" << g_module_size << std::dec
                           << ") RET simulasyonu ATLANIYOR, normal CRASH raporuna dusuluyor.";
                    LOG_ERROR(bad_ss.str());

                    // TANI: Bu fault hangi thread'de olustu ve RSP civarinda ne var?
                    std::stringstream diag_ss;
                    diag_ss << "[TANI] Bu fault TID=" << std::dec << GetCurrentThreadId()
                            << " | RSP=0x" << std::hex << ctx->Rsp << " | RSP civari:";
                    for (int di = -4; di <= 4; di++) {
                        uint64_t* slot = rsp_ptr + di;
                        if (SafeReadable(slot, sizeof(uint64_t))) {
                            diag_ss << "\n    RSP" << (di >= 0 ? "+" : "") << (di * 8) << ": 0x" << *slot;
                        }
                    }
                    diag_ss << std::dec;
                    LOG_ERROR(diag_ss.str());
                    // Asagiya devam et, normal CRASH loglari basilsin (bilerek RET simule etmiyoruz)
                } else {
                    // Varsayilan olarak 0 (basari / NULL) donuyoruz - AMA yukarida
                    // ozel olarak ele alinan (gercek bellek donduren) fonksiyonlar
                    // icin RAX'a zaten dokunulmus, onu ezmiyoruz.
                    if (!special_return_set) {
                        ctx->Rax = 0;
                        // HIZLI YOL isaretleme: bu PLT'yi HICBIR handler
                        // sahiplenmedi (sonuc sadece RAX=0). Isimsiz NID'ler
                        // icin bunu onbellege alip sonraki cagrilarda tum
                        // zinciri atliyoruz -> davranis BIREBIR ayni, ama
                        // string karsilastirmasi yok. (Or. QOQtbeDqsT4 =
                        // cagrilarin ~%25'i.) PLT#8'i haric tutuyoruz: onun
                        // zincir DISINDA ozel yakalamasi var.
                        // DIKKAT: bunu "isimli olsun olmasin hepsi" haline getirmek
                        // DENENDI ve GERI ALINDI - grafik init'inde takilmaya yol
                        // acti (bazi fonksiyonlarin ilk cagrisi zincirden gecmeli).
                        // Yalnizca ISIMSIZ NID'ler guvenli: onlarin hicbir handler'i
                        // yok ve olamaz. Sik cagrilan isimli no-op'lar asagida
                        // (fop cozumlemesinde) ACIKCA hizli yola alinir.
                        if (readable_name.empty() && plt_index != 8 &&
                            plt_index < kPltCacheMax && s_fop[plt_index] == FOP_NONE) {
                            s_fop[plt_index] = FOP_RET0;
                        }
                    }

                    if (log_this) {
                        std::stringstream ret_ss;
                        ret_ss << "[PLT-RET] RAX=0x" << std::hex << ctx->Rax << " (RET: 0x" << ret_addr << ")" << std::dec;
                        LOG_INFO(ret_ss.str());
                    }

                    // Yigin kanaryasi: bu HLE cagrisi kaydedilmis r15'in
                    // uzerine yazdi mi?
                    if (g_canary_addr != 0 && g_canary_hits < 12 &&
                        SafeReadable(reinterpret_cast<void*>(g_canary_addr), 8)) {
                        uint64_t cv = *reinterpret_cast<uint64_t*>(g_canary_addr);
                        if (cv != g_canary_val) {
                            g_canary_hits++;
                            std::stringstream cs;
                            cs << "[KANARYA] BOZULDU! 0x" << std::hex << g_canary_addr
                               << ": 0x" << g_canary_val << " -> 0x" << cv
                               << "  | bunu yapan HLE: " << readable_name
                               << " (PLT#" << std::dec << plt_index << ")";
                            LOG_ERROR(cs.str());
                            g_canary_val = cv;
                        }
                    }

                    // RET komutu simulasyonu
                    ctx->Rip = ret_addr;
                    ctx->Rsp += 8;

                    return EXCEPTION_CONTINUE_EXECUTION;
                }
        }

        // ================================================================
        // Non-PLT EXEC violation handler (Harici kÃ¼tÃ¼phane atlamalarÄ±)
        // ================================================================
        if (access_type != 0 && access_type != 1) { // EXEC violation
            std::stringstream nplt_ss;
            nplt_ss << "[PRX-HLE] Non-PLT EXEC Violation yakalandi @ 0x" << std::hex << access_addr;
            LOG_INFO(nplt_ss.str());

            // Donguye takilma tespiti: HLE stub'lari her zaman RAX=0 dondurdugu icin,
            // oyun kodu gercek bir yan etki (ornegin bir GNM komut tamponu imlecinin
            // ilerlemesi) beklediginde ayni birkac adres arasinda sonsuz retry yapabilir.
            // Son gorulen 2 farkli fault adresini takip edip, uzun sure sadece bu ikisi
            // arasinda gidip geliyorsak "takildik" kabul edip sahte RET simulasyonunu
            // durduruyoruz (asagida normal CRASH/STACK DUMP raporuna dusuyoruz).
            static uint64_t s_recent_fault_addrs[2] = { 0, 0 };
            static int s_stuck_counter = 0;
            constexpr int STUCK_LIMIT = 200;

            bool seen_recently = (access_addr == s_recent_fault_addrs[0] || access_addr == s_recent_fault_addrs[1]);
            if (access_addr != s_recent_fault_addrs[0]) {
                s_recent_fault_addrs[1] = s_recent_fault_addrs[0];
                s_recent_fault_addrs[0] = access_addr;
            }
            s_stuck_counter = seen_recently ? (s_stuck_counter + 1) : 0;

            if (s_stuck_counter >= STUCK_LIMIT) {
                std::stringstream stuck_ss;
                stuck_ss << "[-] DONGU TESPIT EDILDI: Non-PLT EXEC yakalayicisi 0x" << std::hex
                         << s_recent_fault_addrs[0] << " / 0x" << s_recent_fault_addrs[1] << std::dec
                         << " arasinda " << s_stuck_counter << "+ kez sicradi, ilerleme yok. "
                         << "HLE stub'lari (hep RAX=0) oyunun bekledigi gercek yan etkiyi saglayamiyor "
                         << "olabilir. Sahte RET simulasyonu durduruluyor.";
                LOG_ERROR(stuck_ss.str());
                s_stuck_counter = 0; // Rapor bir kez basildiktan sonra sayaci sifirla
                // Asagiya devam et, normal CRASH loglari basilsin (bilerek RET simule etmiyoruz)
            } else {
                // x86_64 RET komutu simulasyonu (Eger call yapildiysa stackte donus adresi vardir)
                uint64_t* rsp_ptr = reinterpret_cast<uint64_t*>(ctx->Rsp);
                uint64_t ret_addr = (SafeReadable(rsp_ptr, sizeof(uint64_t))) ? *rsp_ptr : 0;

                // Ayni guvenlik kontrolu: donus adresi oyun modulunun disindaysa
                // (ornegin call/jmp derinligi desenkronize oldugundan) kor RET yerine
                // acikca hata raporla.
                if (ret_addr != 0 && IsInModuleRange(ret_addr)) {
                    std::stringstream ret_ss;
                    ret_ss << "[PLT-HOOK] HLE Stub calisti -> Return RAX=0 | RET to: 0x" << std::hex << ret_addr;
                    LOG_INFO(ret_ss.str());

                    ctx->Rax = 0; // Basarili donus degeri
                    ctx->Rip = ret_addr;
                    ctx->Rsp += 8;
                    return EXCEPTION_CONTINUE_EXECUTION;
                } else {
                    std::stringstream bad_ss;
                    bad_ss << "[-] HATA: Non-PLT RET adresi (0x" << std::hex << ret_addr
                           << ") gecersiz veya oyun modulunun disinda, RET simule edilemiyor!" << std::dec;
                    LOG_ERROR(bad_ss.str());
                    // Asagiya devam et, normal CRASH loglari basilsin
                }
            }
        }

        ss_init() << " | " << (access_type == 0 ? "READ" : (access_type == 1 ? "WRITE" : "EXEC"))
           << " violation @ 0x" << access_addr;

        // Faulting komutun kendi baytlarini goster - tahmin yerine gercek veriyle
        // ilerleyebilmek icin. RIP oyun modulu icindeyse (yani gercekten calisan
        // bir komuttan kaynaklaniyorsa, sentinel/unmapped bir hedef degilse) anlamli.
        if (IsInModuleRange(ctx->Rip) && SafeReadable(reinterpret_cast<void*>(ctx->Rip), 16)) {
            const uint8_t* rip_bytes = reinterpret_cast<const uint8_t*>(ctx->Rip);
            ss_init() << "\n[-] Faulting komut baytlari @ RIP (RVA: 0x" << (ctx->Rip - g_base_addr) << "): ";
            for (int bi = 0; bi < 16; bi++) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X ", rip_bytes[bi]);
                ss_init() << buf;
            }
        }

        // Stack Dump (READ/WRITE/EXEC farketmeksizin - RVA'yi cozup gercek komutu
        // gormek icin call zincirini takip etmek her turlu ihlalde faydali)
        {
            uint64_t* stack_ptr = reinterpret_cast<uint64_t*>(ctx->Rsp);
            ss_init() << "\n[-] --- STACK DUMP ---";
            for (int i = 0; i < 16; i++) {
                if (SafeReadable(stack_ptr + i, sizeof(uint64_t))) {
                    uint64_t val = stack_ptr[i];
                    ss_init() << "\n    RSP+" << std::hex << (i * 8) << ": 0x" << val;
                    if (IsInTextSegment(val)) {
                        ss_init() << " [<-- VALID CODE OFFSET: 0x" << (val - g_base_addr) << "]";
                    }
                } else {
                    ss_init() << "\n    RSP+" << std::hex << (i * 8) << ": [INACCESSIBLE]";
                    break; // Bellek erisilemez ise asagiya inmeye gerek yok
                }
            }
            ss_init() << "\n[-] ------------------";
        }
    }

    // Genel backtrace (HER cokme tipi icin, ozellikle STACK OVERFLOW):
    // RSP'den yukari tarayip oyun modulune ait donus adreslerini RVA olarak
    // topla. Ayni RVA'nin tekrar tekrar gorunmesi = OZYINELEME (stack overflow).
    {
        uint64_t* sp = reinterpret_cast<uint64_t*>(ctx->Rsp);
        ss_init() << "\n[-] [backtrace RVA]";
        int found = 0;
        for (int i = 0; i < 256 && found < 16; i++) {
            if (!SafeReadable(sp + i, sizeof(uint64_t))) break;
            uint64_t v = sp[i];
            if (v >= g_base_addr && v < g_base_addr + g_module_size) {
                ss_init() << " 0x" << std::hex << (v - g_base_addr);
                found++;
            }
        }
    }

    if (g_trace_steps != 0) {
        ss_init() << "\n[-] [TRACE] son " << std::dec
                  << (g_trace_pos < (uint64_t)kTraceRing ? g_trace_pos : (uint64_t)kTraceRing)
                  << " komut (toplam " << g_trace_steps << " adim), RVA olarak:\n[-]  ";
        uint64_t cnt = g_trace_pos < (uint64_t)kTraceRing ? g_trace_pos : (uint64_t)kTraceRing;
        for (uint64_t i = 0; i < cnt; i++) {
            uint64_t v = g_trace_ring[(g_trace_pos - cnt + i) % kTraceRing];
            ss_init() << " 0x" << std::hex << (v - g_base_addr);
            if ((i % 8) == 7) ss_init() << "\n[-]  ";
        }
    }

    if (g_r15_pos != 0) {
        uint64_t cnt = g_r15_pos < (uint64_t)kR15Ring ? g_r15_pos : (uint64_t)kR15Ring;
        ss_init() << "\n[-] [R15-ARGV] argv'nin r15'e girip ciktigi son "
                  << std::dec << cnt << " an (toplam " << g_r15_pos << "):";
        for (uint64_t i = 0; i < cnt; i++) {
            const R15Event& e = g_r15_ring[(g_r15_pos - cnt + i) % kR15Ring];
            ss_init() << "\n[-]   RVA 0x" << std::hex << (e.rip - g_base_addr)
                      << "  r15: 0x" << e.from << " -> 0x" << e.to;
        }
    }

    if (g_initcall_addr != 0) {
        ss_init() << "\n[-] [INIT-TRACE] son calisan .init_array girdisi: #"
                  << std::dec << g_initcall_n << " RVA 0x" << std::hex << g_initcall_last
                  << " (imlec RVA 0x" << g_initcall_cursor << ")";
    }

    ss_init() << std::dec;
    LOG_ERROR(ss_init().str());

    // Register dokumu
    std::stringstream regs;
    regs << std::hex
         << "  RAX=0x" << ctx->Rax << " RBX=0x" << ctx->Rbx
         << " RCX=0x" << ctx->Rcx << " RDX=0x" << ctx->Rdx << "\n"
         << "  RSI=0x" << ctx->Rsi << " RDI=0x" << ctx->Rdi
         << " RSP=0x" << ctx->Rsp << " RBP=0x" << ctx->Rbp << "\n"
         << "  R8=0x"  << ctx->R8  << " R9=0x"  << ctx->R9
         << " R10=0x" << ctx->R10 << " R11=0x" << ctx->R11 << "\n"
         << "  R12=0x" << ctx->R12 << " R13=0x" << ctx->R13
         << " R14=0x" << ctx->R14 << " R15=0x" << ctx->R15;
    LOG_ERROR(regs.str());

    // Tip-kayit fonksiyonu (0x2dfff0) kac kez cagrildi? Sayac 4-slot
    // tabloyu tasiriyorsa, bu deger beklenenden (<=4) fazla olmali.
    if (g_reg_call_count_ptr != nullptr) {
        std::stringstream rc;
        rc << "[TANI] 0x2dfff0 (tip-kayit) toplam cagri sayisi = "
           << *g_reg_call_count_ptr;
        LOG_ERROR(rc.str());
    }

    // NULL sanal cagri tanisi: rax=[rdi], call [rax+X] deseninde rdi=0 ise
    // "hangi singleton null?" sorusunu cevaplamak icin R12'nin (cogunlukla
    // singleton pointer'i tutar) modul icinde mi yoksa cop mu oldugunu ve
    // isaret ettigi bellegi dok.
    if (ctx->Rdi == 0 && IsInModuleRange(ctx->Rip)) {
        std::stringstream ns;
        ns << "[CRASH-TANI] NULL nesne. ";
        uint64_t r12 = ctx->R12;
        bool r12_in_mod = (r12 >= g_base_addr && r12 < g_base_addr + g_module_size);
        ns << "R12=0x" << std::hex << r12
           << (r12_in_mod ? "  (MODUL ICINDE -> gecerli, constructor calismamis)"
                          : "  (modul DISI -> relocation/cop olabilir)");
        if (r12 != 0 && SafeReadable(reinterpret_cast<void*>(r12), 16)) {
            uint64_t o0 = *reinterpret_cast<uint64_t*>(r12);
            uint64_t o8 = *reinterpret_cast<uint64_t*>(r12 + 8);
            ns << "  *R12=0x" << o0 << " *(R12+8)=0x" << o8;
        }
        LOG_ERROR(ns.str());
    }

    // Windows standart hata isleyicisine devam etsin
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI Core::ExecutionThread(LPVOID lpParam) {
    g_game_tid = GetCurrentThreadId(); // PLT-TOP duvar saati olcumu icin
    // TLS blogunu/TEB slotunu misafir koda girmeden hazirla: fs:[0] komutlari
    // yamalandiktan sonra artik fault etmiyor, yani tembel kurulum yetmez.
    GetThreadTlsBase();

    uint64_t entry_point = reinterpret_cast<uint64_t>(lpParam);
    uint64_t procparam_vaddr = g_procparam_vaddr;

    std::stringstream ss;
    ss << "Execution Thread baslatildi! module_start -> 0x" << std::hex << entry_point << std::dec;
    LOG_INFO(ss.str());

    // ================================================================
    // ADIM 0.5: Dinamik Analiz - module_start icindeki E8 Call'u Bul
    // ================================================================
    uint8_t* code_ptr = reinterpret_cast<uint8_t*>(entry_point);
    
    // 1) module_start'in ilk 256 byte'ini hex olarak yazdir
    {
        std::stringstream hd;
        hd << "\n[DEBUG] module_start (0x" << std::hex << entry_point << ") ilk 256 byte:\n";
        for (int i = 0; i < 256; ++i) {
            if (i % 16 == 0 && i != 0) hd << "\n";
            hd << std::hex << std::setw(2) << std::setfill('0') << (int)code_ptr[i] << " ";
        }
        hd << "\n";
        LOG_INFO(hd.str());
    }

    uint64_t actual_main_ptr = 0;
    bool pattern_found = false;

    // Sadece ilk 64 byte icinde arayalim
    for (int i = 0; i < 64; ++i) {
        if (code_ptr[i] == 0x55 && 
            code_ptr[i+1] == 0x48 && 
            code_ptr[i+2] == 0x89 && 
            code_ptr[i+3] == 0xE5 && 
            code_ptr[i+4] == 0xE8) {
            
            // E8 komutundan sonraki 4 byte'i signed 32-bit offset olarak oku
            int32_t rel_offset = *reinterpret_cast<int32_t*>(&code_ptr[i+5]);
            
            // E8 komutunun toplam uzunlugu 5 byte (E8 + 4 byte offset).
            // RIP, bir sonraki komutun adresine isaret eder (entry_point + i + 9).
            uint64_t next_ip = entry_point + i + 9;
            actual_main_ptr = next_ip + rel_offset;
            
            pattern_found = true;
            break;
        }
    }

    if (pattern_found) {
        std::stringstream ms;
        ms << "[!!!] MODULE_START ICINDEKI GIZLI MAIN BULUNDU -> 0x" << std::hex << actual_main_ptr;
        LOG_INFO(ms.str());
        
        // 2) Gercek main pointer'in ilk 64 byte'ini hex olarak yazdir
        std::stringstream mhd;
        mhd << "\n[DEBUG] Gizli Main (0x" << std::hex << actual_main_ptr << ") ilk 64 byte:\n";
        uint8_t* main_code_ptr = reinterpret_cast<uint8_t*>(actual_main_ptr);
        for (int i = 0; i < 64; ++i) {
            if (i % 16 == 0 && i != 0) mhd << "\n";
            mhd << std::hex << std::setw(2) << std::setfill('0') << (int)main_code_ptr[i] << " ";
        }
        mhd << "\n";
        LOG_INFO(mhd.str());
        
        // OYUN DOGAL AKISINDA CALISSIN DIYE YONLENDIRMEYI IPTAL ETTIK
        // entry_point = actual_main_ptr;
    } else {
        LOG_INFO("[-] UYARI: module_start icinde E8 pattern'i bulunamadi. Orijinal adres kullanilacak.");
    }

    // ================================================================
    // ADIM 0.8: GERCEK ProcessParam'in Incelenmesi ve Taranmasi
    // ================================================================
    if (procparam_vaddr != 0) {
        uint64_t target_addr = g_base_addr + procparam_vaddr;
        std::stringstream ss;
        ss << "[INFO] PT_SCE_PROCPARAM bellek dÃ¶kÃ¼mÃ¼ (0x" << std::hex << target_addr << ") ilk 96 byte:\n";
        uint8_t* dump_ptr = reinterpret_cast<uint8_t*>(target_addr);
        for (int i = 0; i < 96; i++) {
            if (i > 0 && i % 16 == 0) ss << "\n";
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)dump_ptr[i] << " ";
        }
        ss << "\n";
        LOG_INFO(ss.str());
    }

    {
        uint8_t* scan_base = reinterpret_cast<uint8_t*>(g_base_addr);
        size_t scan_size = 32 * 1024 * 1024; // 32 MB max
        uint64_t found_block_array = 0;
        
        LOG_INFO("[SCANNER] Gercek ProcessParam block_array araniyor (0x6AC156EF)...");
        // HIZ: SafeReadable'i her 4 baytta degil, SAYFA basina bir kez cagir.
        // Onceki hali 32MB icin ~8M SafeReadable (VirtualQuery/IsBadReadPtr)
        // yapip acilisi saniyelerce bekletiyordu.
        for (size_t base = 0; base + 24 <= scan_size && found_block_array == 0; base += 0x1000) {
            size_t want = (scan_size - base < 0x1000 + 16) ? (scan_size - base) : (0x1000 + 16);
            if (!SafeReadable(&scan_base[base], want)) {
                break; // sayfa okunamiyor -> tarama bolgesinin sonu
            }
            size_t end = (base + 0x1000 < scan_size - 24) ? (base + 0x1000) : (scan_size - 24);
            for (size_t i = base; i < end; i += 4) {
                if (*reinterpret_cast<uint32_t*>(&scan_base[i]) == 0x6AC156EF &&
                    *reinterpret_cast<uint32_t*>(&scan_base[i + 12]) == 0x6AC15610) {
                    found_block_array = g_base_addr + i;
                    std::stringstream ss;
                    ss << "[SCANNER] block_array bulundu! Adres: 0x" << std::hex << found_block_array;
                    LOG_INFO(ss.str());
                    break;
                }
            }
        }
        
        if (found_block_array != 0) {
            LOG_INFO("[SCANNER] ProcessParam yapisi icin tersine pointer taramasi basliyor...");
            // HIZ: yine sayfa basina bir kez okunabilirlik kontrolu.
            for (size_t base = 0; base + 8 <= scan_size && g_real_process_param == 0; base += 0x1000) {
                size_t want = (scan_size - base < 0x1000) ? (scan_size - base) : 0x1000;
                if (!SafeReadable(&scan_base[base], want)) {
                    break;
                }
                size_t end = (base + 0x1000 < scan_size - 8) ? (base + 0x1000) : (scan_size - 8);
                for (size_t i = base; i < end; i += 8) {
                    uint64_t ptr = *reinterpret_cast<uint64_t*>(&scan_base[i]);
                    if (ptr == found_block_array && i >= 0x30) {
                        g_real_process_param = g_base_addr + i - 0x30;
                        std::stringstream ss;
                        ss << "[!!!] GERCEK ProcessParam BULUNDU! Adres: 0x" << std::hex << g_real_process_param;
                        LOG_INFO(ss.str());
                        break;
                    }
                }
            }
        } else {
            LOG_INFO("[-] UYARI: block_array bulunamadi! Oyun hafizasinda 0x6AC156EF dizilimi yok.");
        }
    }
    
    // Eger tarama basarisiz olduysa ve ELF ProcessParam verdiyse onu kullanalim
    if (g_real_process_param == 0 && procparam_vaddr != 0) {
        g_real_process_param = g_base_addr + procparam_vaddr;
        LOG_INFO("[INFO] Scanner bulamadi, PT_SCE_PROCPARAM adresi zorla kullaniliyor.");
    }

    // ================================================================
    // YARDIMCI: System V AMD64 ABI Trampoline Olusturucu
    // ================================================================
    // PS4/PS5 kodu System V ABI bekliyor (RDI, RSI, RDX arguman register'lari).
    // MSVC inline assembly desteklemedigi icin calisma zamaninda makine kodu
    // uretip VirtualAlloc PAGE_EXECUTE_READWRITE bir bloÄŸa yaziyoruz.
    //
    // Uretilen stub:
    //   mov rdi, <arg0>         ; 48 BF + 8 byte imm
    //   mov rsi, <arg1>         ; 48 BE + 8 byte imm  
    //   mov rdx, <arg2>         ; 48 BA + 8 byte imm
    //   mov rax, <target>       ; 48 B8 + 8 byte imm
    //   jmp rax                 ; FF E0
    // Toplam: 4*(2+8) + 2 = 42 byte
    auto BuildSysVTrampoline = [](uint64_t target, uint64_t rdi_val, uint64_t rsi_val, uint64_t rdx_val) -> void* {
        uint8_t* stub = reinterpret_cast<uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!stub) return nullptr;
        int off = 0;
        // mov rdi, imm64
        stub[off++] = 0x48; stub[off++] = 0xBF;
        memcpy(&stub[off], &rdi_val, 8); off += 8;
        // mov rsi, imm64
        stub[off++] = 0x48; stub[off++] = 0xBE;
        memcpy(&stub[off], &rsi_val, 8); off += 8;
        // mov rdx, imm64
        stub[off++] = 0x48; stub[off++] = 0xBA;
        memcpy(&stub[off], &rdx_val, 8); off += 8;
        // mov rax, imm64
        stub[off++] = 0x48; stub[off++] = 0xB8;
        memcpy(&stub[off], &target, 8); off += 8;
        // jmp rax
        stub[off++] = 0xFF; stub[off++] = 0xE0;
        return stub;
    };

    typedef int(*TrampolineFunc)();

    // ================================================================
    // ADIM 0.9: DT_INIT - CRT Baslatici (.init_array yurutucusu)
    // ================================================================
    // module_start'tan ONCE cagirilmasi sarttir; DT_INIT_ARRAY dinamik
    // etiketi bu ELF'te BOS olsa da, DT_INIT (RVA'si buradan gelir) statik
    // baglanmis binary'lerde tipik olarak .init_array'i RIP-relative gomulu
    // pointer'lar uzerinden KENDI ICINDE tarayip her constructor'i cagiran
    // klasik crtbegin.o "_init" fonksiyonudur. Bu adres daha once HICBIR
    // ZAMAN cagirilmiyordu - RVA 0x2c61b2 cokmesindeki gibi initialize
    // edilmemis global/static objelerin gercek kok nedeni buydu.
    // OYUNA OZEL. PS5'te e_entry (_start) ZATEN .init_array'i kendisi
    // yurutur; DT_INIT'i ayrica cagirmak statik ilklendiricileri IKI KEZ
    // calistirir. Dreaming Sarah'ta bu gerekliydi (onun _start'i init'i
    // calistirmiyor; olculdu - eklenmeden once initialize edilmemis globaller
    // yuzunden cokuyordu), ama Astro Bot'ta ZARARLI:
    //   Statik kayit fonksiyonu iki kez calisinca ayni dugumu listeye IKINCI
    //   kez ekliyor. Ekleme "listenin sonunu bul, [tail]=node" seklinde
    //   oldugu icin dugum KENDI next'ine kendini yaziyor (node->next = node)
    //   ve sonraki liste yuruyusu SONSUZ DONGUYE giriyor.
    //   Olculdu: RVA 0x7426c03'te tam 1.00 cekirdek spin, watchdog'da
    //   RDX+0x10 == RCX ve degerler her ornekte ayni.
    if (g_init_vaddr != 0 && Game::Current().quirk_call_dt_init) {
        uint64_t init_entry = g_base_addr + g_init_vaddr;
        void* init_trampoline = BuildSysVTrampoline(init_entry, 0, 0, 0);

        std::stringstream is_ss;
        is_ss << "[ADIM 0.9] DT_INIT cagriliyor (CRT/.init_array yurutucusu): 0x"
              << std::hex << init_entry << std::dec;
        LOG_INFO(is_ss.str());

        TrampolineFunc init_tramp = reinterpret_cast<TrampolineFunc>(init_trampoline);
        init_tramp();

        LOG_INFO("[ADIM 0.9] DT_INIT tamamlandi.");
    } else {
        LOG_INFO("[-] UYARI: DT_INIT bulunamadi, CRT baslatici atlaniyor.");
    }

    // ================================================================
    // ADIM 1: Oyunun (module_start) System V ABI ile Baslatilmasi
    // ================================================================
    // ================================================================
    // PS4/Orbis argc/argv Yapisi Olustur
    // ================================================================
    // CRT _start fonksiyonu RDI'den su yapiyi bekliyor:
    //   [rdi+0x00] = int32 argc
    //   [rdi+0x08] = char* argv[0]  (program adi)
    //   [rdi+0x10] = char* argv[1]  (NULL terminator)
    //   [rdi+0x18] = char* envp[0]  (NULL terminator)
    //
    // RSI ise ayri bir parametre (ProcessParam veya aux vektoru olabilir, simdilik 0)
    uint8_t* args_block = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    
    // Program adi stringi (argv[0])
    const char* prog_name = "eboot.bin";
    char* name_storage = reinterpret_cast<char*>(args_block + 0x100);
    strcpy(name_storage, prog_name);
    
    // argc = 1
    *reinterpret_cast<int32_t*>(args_block + 0x00) = 1;
    // argv[0] = pointer to "eboot.bin"
    *reinterpret_cast<uint64_t*>(args_block + 0x08) = reinterpret_cast<uint64_t>(name_storage);
    // argv[1] = NULL (terminator)
    *reinterpret_cast<uint64_t*>(args_block + 0x10) = 0;
    // envp[0] = NULL (terminator)
    *reinterpret_cast<uint64_t*>(args_block + 0x18) = 0;
    // module_start "r15 = rdi + 8" ile argv'yi kuruyor; tani araclari bunu
    // dogrulayabilsin diye beklenen degeri saklıyoruz.
    g_expected_argv = reinterpret_cast<uint64_t>(args_block) + 8;
    
    void* trampoline1 = BuildSysVTrampoline(
        entry_point,
        reinterpret_cast<uint64_t>(args_block),         // RDI = argc/argv blogu
        0,                                               // RSI = 0 (simdilik)
        0                                                // RDX = 0
    );
    
    {
        std::stringstream cs;
        cs << "[ADIM 1] Trampoline ile e_entry cagriliyor (Entry: 0x" << std::hex << entry_point 
           << ", RDI=0x" << reinterpret_cast<uint64_t>(args_block)
           << " [argc=1, argv={\"eboot.bin\"}])...";
        LOG_INFO(cs.str());
    }

    TrampolineFunc tramp1 = reinterpret_cast<TrampolineFunc>(trampoline1);
    int result = tramp1();
    
    {
        std::stringstream rs;
        rs << "[ADIM 1] module_start() tamamlandi! Donus degeri: " << result;
        LOG_INFO(rs.str());
    }

    // ================================================================
    // ADIM 2: GERCEK GAME THREAD ENTRY'YI CIKAR VE BASLAT
    // ================================================================
    if (g_real_process_param != 0) {
        uint64_t* param_ptr = reinterpret_cast<uint64_t*>(g_real_process_param);
        SceProcessParamBlock* blocks = reinterpret_cast<SceProcessParamBlock*>(param_ptr[6]); // +0x30 block_array
        uint64_t block_count = param_ptr[7]; // +0x38 block_count
        
        uint64_t real_game_entry = 0;
        
        for (uint64_t i = 0; i < block_count; i++) {
            if (blocks[i].magic == 0x6AC156EF) {
                uint32_t data1 = blocks[i].data1;
                uint32_t edi = data1 & 3;
                uint32_t esi = data1 & 0x3FC;
                
                uint64_t base_ptr = param_ptr[edi];
                if (base_ptr != 0) {
                    uint64_t final_ptr_addr = base_ptr + (esi * 2);
                    uint64_t final_ptr = *reinterpret_cast<uint64_t*>(final_ptr_addr);
                    if (final_ptr != 0) {
                        real_game_entry = *reinterpret_cast<uint64_t*>(final_ptr);
                        break;
                    }
                }
            }
        }
        
        if (real_game_entry != 0) {
            std::stringstream ss;
            ss << "\n=============================================\n";
            ss << "[!!!] GERCEK GAME MAIN (Asil Oyun Dongusu) COZULDU: 0x" << std::hex << real_game_entry << "\n";
            ss << "=============================================\n";
            LOG_INFO(ss.str());
            
            // Asil oyunu System V ABI trampoline ile baslat
            void* trampoline2 = BuildSysVTrampoline(
                real_game_entry,
                0,                                              // RDI = 0
                reinterpret_cast<uint64_t>(args_block),         // RSI = guvenli bellek
                0                                               // RDX = 0
            );
            
            LOG_INFO("[ADIM 2] Trampoline ile Game Main cagriliyor...");
            TrampolineFunc tramp2 = reinterpret_cast<TrampolineFunc>(trampoline2);
            tramp2();
            LOG_INFO("[ADIM 2] Game Main bitti.");
        } else {
            LOG_INFO("[-] UYARI: Gercek ProcessParam icinde 0x6AC156EF magic veya gecerli pointer bulunamadi.");
        }
    } else {
        LOG_INFO("[-] UYARI: Gercek ProcessParam olmadigi icin ADIM 2 atlandi.");
    }

    LOG_INFO("Execution Thread sonlandi.");
    return 0;
}

void Core::StartExecution(uint64_t entry_point, uint64_t base_addr, uint64_t text_size, uint64_t original_entry, uint64_t procparam_vaddr,
                           uint64_t tls_vaddr, uint64_t tls_filesz, uint64_t tls_memsz, uint64_t tls_align, uint64_t module_size,
                           uint64_t init_vaddr) {
    // Global degiskenleri ayarla (VEH icinden erisilebilmesi icin)
    g_base_addr = base_addr;
    g_text_size = text_size;
    g_module_size = module_size;
    g_init_vaddr = init_vaddr;
    g_game_thread_entry = 0;
    g_plt8_param_ptr = 0;
    g_plt8_param_size = 0;
    g_original_entry = original_entry;
    g_real_process_param = 0;
    g_procparam_vaddr = procparam_vaddr;

    g_tls_vaddr = tls_vaddr;
    g_tls_filesz = tls_filesz;
    g_tls_memsz = tls_memsz;
    // Thread basina blok uretebilmek icin sablonu sakla
    g_tls_align_v      = tls_align;
    g_tls_template_src = (tls_vaddr != 0) ? (base_addr + tls_vaddr) : 0;
    g_tls_align = tls_align;
    g_tls_base = 0;

    // ================================================================
    // Gercek TLS (Thread-Local Storage) Blogunun Olusturulmasi
    // ================================================================
    // x86_64 ELF TLS modeli (Variant II): TLS verisi (.tdata/.tbss) thread
    // pointer'in (tp) HEMEN ONCESINE yerlestirilir ve koddaki "mov reg, fs:[0]"
    // erisimleri tp'yi okuduktan sonra (tp - offset) seklinde negatif offsetlerle
    // .tdata/.tbss alanina ulasir. tp'nin kendisi de kendi adresini gosterir
    // (self-pointer kurali: *(tp) = tp).
    if (tls_memsz > 0) {
        uint64_t align = tls_align ? tls_align : 8;
        // p_align'a gore yukari hizala (ELF'te align her zaman 2'nin kuvvetidir)
        uint64_t aligned_size = (tls_memsz + (align - 1)) & ~(align - 1);

        // Gercek TCB (Thread Control Block), sadece 8 byte'lik bir self-pointer
        // degildir: ABI'de tp'nin ilerisinde (tp+8, tp+0x10, ...) kullanilan
        // rezerve bir alan (dtv pointer, guard degerleri vb. icin) bulunur. Bu
        // alani ayirmazsak, kod bu offsetlere eristiginde bizim blogumuzun disina
        // tasip rastgele bellek/NULL okuyabilir. Referans (KytyPS5) TCB icin
        // 0x40 byte, 0x20 hizali bir alan kullaniyor; ayni yaklasimi izliyoruz.
        constexpr uint64_t TCB_SIZE = 0x40;
        constexpr uint64_t TCB_ALIGN = 0x20;
        uint64_t tcb_offset = aligned_size;
        uint64_t total_size = ((tcb_offset + (TCB_ALIGN - 1)) & ~(TCB_ALIGN - 1)) + TCB_SIZE;

        uint8_t* tls_block = reinterpret_cast<uint8_t*>(
            VirtualAlloc(nullptr, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

        if (tls_block) {
            memset(tls_block, 0, total_size);

            // .tdata sablonunu kopyala (segment zaten normal PT_LOAD kopyasiyla
            // base_addr + tls_vaddr adresinde bellekte hazir durumda)
            if (tls_filesz > 0 && tls_vaddr != 0) {
                memcpy(tls_block, reinterpret_cast<void*>(base_addr + tls_vaddr), tls_filesz);
            }

            g_tls_base = reinterpret_cast<uint64_t>(tls_block) + tcb_offset;
            *reinterpret_cast<uint64_t*>(g_tls_base) = g_tls_base; // Variant II: *(tp) = tp
            // tp'nin ilerisindeki kalan TCB alani (tp+8 .. tp+0x38) sifirlanmis
            // durumda birakiliyor; yukaridaki memset bunu zaten garanti ediyor.

            std::stringstream tss;
            tss << "[TLS] Gercek TLS blogu olusturuldu! block=0x" << std::hex << reinterpret_cast<uint64_t>(tls_block)
                << " tp=0x" << g_tls_base << " aligned_size=0x" << aligned_size
                << " tcb_reserved=0x" << TCB_SIZE << " total_alloc=0x" << total_size
                << " filesz=0x" << tls_filesz << " memsz=0x" << tls_memsz
                << " align=0x" << align << std::dec;
            LOG_INFO(tss.str());
        } else {
            LOG_ERROR("[-] HATA: TLS blogu icin bellek ayrilamadi!");
        }
    } else {
        LOG_INFO("[INFO] PT_TLS segmenti bulunamadi, TLS blogu olusturulmadi.");
    }

    LOG_INFO("Vectored Exception Handler (VEH) kaydediliyor...");
    
    // Ilk sirada cagrilmasi icin 1 (TRUE) veriyoruz
    // fs:[0] hizli yolu icin Windows TLS slotunu VEH'ten ONCE ayir: VEH
    // icinde TlsAlloc gibi kilit alan is yapmiyoruz.
    PsemuInitTlsFastPath();

    // DENENDI VE GERI ALINDI: timeBeginPeriod(1) + usleep icin yuksek
    // cozunurluklu bekleme zamanlayicisi. Beklenti kare suresini dusurmekti;
    // OLCUM TERSINI SOYLEDI: FPS 15 -> 11-13'e dustu ve PLT hizi da dustu
    // (39-40k -> 29-37k/sn), yani sistem topluca yavasladi. Oyunun usleep
    // istekleri zaten hep tam 1000 us ve cagri sayisi degismedi; uykuyu
    // kisaltmak kareyi hizlandirmadi, muhtemelen o thread'i daha sik uyandirip
    // oyun thread'inden CPU caldi.

    // YUKLEME PROFILI: PLT-TOP dokumu eskiden yalnizca present yolundan
    // cagriliyordu, yani ancak render basladiktan SONRA veri veriyordu. Oysa
    // yuklemenin buyuk kismi render baslamadan once geciyor (zaman damgalarina
    // gore 64 sn ve 51 sn'lik sessiz araliklar). Bagimsiz bir thread'den
    // periyodik dokum alarak o araliklarda ne oldugunu goruyoruz.
    // Varsayilan KAPALI: bu thread, oyunun en erken boot asamasinda VEH ile
    // ayni anda printf yapiyor ve isin basinda takilmalarla zaman olarak
    // ortusuyor. Suclu oldugu KANITLANMADI, ama olcum araci oyunu bozuyorsa
    // once olcumu izole etmek gerekir. Acmak icin: PSEMU_LOAD_PROFILE=1
    if (const char* e = getenv("PSEMU_LOAD_PROFILE")) {
        if (e[0] != '0') {
            CreateThread(
                NULL, 0,
                [](LPVOID) -> DWORD {
                    for (;;) {
                        Sleep(3000);
                        PsemuDumpPltTop();
                    }
                },
                nullptr, 0, NULL);
        }
    }

    PVOID veh_handle = AddVectoredExceptionHandler(1, SyscallExceptionFilter);
    if (!veh_handle) {
        LOG_ERROR("VEH kaydedilemedi!");
        return;
    }

    TimeInit(); // saat kaynagini baslat (clock_gettime/gettimeofday icin)

    // NOT: utf16 tani breakpoint'i (0x17b120) DEVRE DISI. Single-step
    // re-arm thread-guvenli degil (birden fazla thread formatlayinca 0xCC
    // yamasi/geri-koyma yarisa giriyor). Bunun yerine utf16 kaynagini
    // __cxa_throw aninda yigin taramasiyla buluyoruz (mudahalesiz).
    (void)g_diag_bp_orig;

    // NOT: SetupWatchpoint(base_addr) DEVRE DISI.
    // 0x4942c8 uzerindeki PAGE_GUARD, cozulmus GLOB_DAT relokasyon hatasini
    // teshis etmek icin kurulmustu. Artik gereksiz ve ZARARLI: koruma tum
    // 0x1000'lik sayfayi erisilemez yapiyor, o sayfayi tarayan CRT
    // fonksiyonlari tetikleniyor (logda 32 isabet) ve hemen ardindan
    // access violation geliyordu. Yeniden gerekirse tek satirla acilir.
    (void)base_addr;

    // .init_array izleyicisi (istege bagli): PSEMU_INIT_TRACE=0x62 gibi,
    // CRT yuruyucusundeki "call rax" komutunun RVA'si verilir.
    if (const char* itr = std::getenv("PSEMU_INIT_TRACE")) {
        uint64_t rva = std::strtoull(itr, nullptr, 0);
        if (rva != 0) {
            uint8_t* p = reinterpret_cast<uint8_t*>(base_addr + rva);
            if (p[0] == 0xFF && p[1] == 0xD0) { // call rax
                DWORD oldp = 0;
                VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &oldp);
                g_initcall_addr = base_addr + rva;
                g_initcall_next = base_addr + rva + 2;
                if (const char* v = std::getenv("PSEMU_INIT_TRACE_LOG"))
                    g_initcall_verbose = (v[0] == '1');
                p[0] = 0xCC;
                LOG_INFO("[INIT-TRACE] .init_array izleyicisi kuruldu (RVA 0x" +
                         std::to_string(rva) + ")");
            } else {
                LOG_ERROR("[INIT-TRACE] RVA 0x" + std::to_string(rva) +
                          " 'call rax' (FF D0) degil, izleyici kurulmadi.");
            }
        }
    }

    // Prologue breakpoint'leri: PSEMU_BP=0x29fb10,0x2b2200 gibi
    if (const char* bpe = std::getenv("PSEMU_BP")) {
        const char* s = bpe;
        while (*s && g_bp_count < 4) {
            char* end = nullptr;
            uint64_t rva = std::strtoull(s, &end, 0);
            if (end == s) break;
            uint8_t* p = reinterpret_cast<uint8_t*>(base_addr + rva);
            if (rva != 0 && p[0] == 0x55) { // push rbp
                DWORD oldp = 0;
                VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldp);
                g_bp_addr[g_bp_count++] = base_addr + rva;
                p[0] = 0xCC;
                std::stringstream bs;
                bs << "[BP] RVA 0x" << std::hex << rva << " kuruldu.";
                LOG_INFO(bs.str());
            } else {
                std::stringstream bs;
                bs << "[BP] RVA 0x" << std::hex << rva
                   << " 'push rbp' (0x55) degil, kurulmadi.";
                LOG_ERROR(bs.str());
            }
            s = (*end == ',') ? end + 1 : end;
        }
    }

    // Bos isaretciye yazmayi atla (bring-up tanisi): PSEMU_SKIP_NULL_STORE=1
    if (const char* sn = std::getenv("PSEMU_SKIP_NULL_STORE")) {
        g_skip_null_store = (sn[0] == '1');
        if (g_skip_null_store)
            LOG_ERROR("[NULL-STORE] atlama ACIK - bu bir TANI kipi, kalici duzeltme degil.");
    }

    // Kod araligini salt-okunur yap: PSEMU_PROTECT_CODE=1
    if (const char* pc = std::getenv("PSEMU_PROTECT_CODE")) {
        if (pc[0] == '1' && g_text_size > 0) {
            DWORD oldp = 0;
            if (VirtualProtect(reinterpret_cast<void*>(base_addr),
                               static_cast<SIZE_T>(g_text_size),
                               PAGE_EXECUTE_READ, &oldp)) {
                g_protect_code = true;
                std::stringstream ps;
                ps << "[KOD-KORUMA] kod araligi salt-okunur yapildi: 0x"
                   << std::hex << base_addr << " + 0x" << g_text_size;
                LOG_ERROR(ps.str());
                // Ayni baytlari MISAFIR HENUZ CALISMADAN dok: boylece
                // bozulmanin yukleme aninda mi yoksa calisirken mi
                // oldugunu ayirt ediyoruz.
                if (const char* dr = std::getenv("PSEMU_DUMP_RVA")) {
                    const char* s = dr;
                    for (int k = 0; k < 6 && *s; k++) {
                        char* end = nullptr;
                        uint64_t rva = std::strtoull(s, &end, 0);
                        if (end == s) break;
                        const uint8_t* q = reinterpret_cast<const uint8_t*>(base_addr + rva);
                        std::stringstream ds;
                        ds << "[YUKLEME-SONRASI] RVA 0x" << std::hex << rva << ": ";
                        for (int i = 0; i < 16; i++) {
                            char b[4];
                            snprintf(b, sizeof(b), "%02X ", q[i]);
                            ds << b;
                        }
                        LOG_ERROR(ds.str());
                        s = (*end == ',') ? end + 1 : end;
                    }
                }
            } else {
                LOG_ERROR("[KOD-KORUMA] VirtualProtect basarisiz.");
            }
        }
    }

    // Donanim yazma izlemesi: PSEMU_WATCH_WRITE=<rva> (PSEMU_BP ilk tetiklendiginde kurulur)
    if (const char* ww = std::getenv("PSEMU_WATCH_WRITE")) {
        uint64_t rva = std::strtoull(ww, nullptr, 0);
        if (rva != 0) {
            if (rva & 3)
                LOG_ERROR("[WATCH-W] RVA 4'e hizali degil, izleme kurulmayacak.");
            else if (g_bp_count == 0)
                LOG_ERROR("[WATCH-W] PSEMU_BP gerekli (izleme orada kuruluyor).");
            else
                g_watchw_addr = base_addr + rva;
        }
    }

    // Yigin kanaryasi: PSEMU_CANARY=<rva> (o RVA ayrica PSEMU_BP'de olmali)
    if (const char* cv = std::getenv("PSEMU_CANARY")) {
        uint64_t rva = std::strtoull(cv, nullptr, 0);
        if (rva != 0) {
            g_canary_bp = base_addr + rva;
            bool armed = false;
            for (int i = 0; i < g_bp_count; i++)
                if (g_bp_addr[i] == g_canary_bp) armed = true;
            if (!armed)
                LOG_ERROR("[KANARYA] UYARI: PSEMU_CANARY icin PSEMU_BP'de ayni "
                          "RVA yok, kanarya kurulmayacak.");
        }
    }

    // Tek adim izleme: PSEMU_TRACE_FROM=<rva> (o RVA ayrica PSEMU_BP'de olmali)
    if (const char* tf = std::getenv("PSEMU_TRACE_FROM")) {
        uint64_t rva = std::strtoull(tf, nullptr, 0);
        if (rva != 0) {
            g_trace_from = base_addr + rva;
            if (const char* tm = std::getenv("PSEMU_TRACE_MAX"))
                g_trace_max = std::strtoull(tm, nullptr, 0);
            bool armed = false;
            for (int i = 0; i < g_bp_count; i++)
                if (g_bp_addr[i] == g_trace_from) armed = true;
            if (!armed)
                LOG_ERROR("[TRACE] UYARI: PSEMU_TRACE_FROM icin PSEMU_BP'de ayni "
                          "RVA yok, izleme baslamayacak.");
        }
    }

    // Hang watchdog thread'ini baslat (worker takilirsa RIP'ini doksun)
    CreateThread(NULL, 0, HangWatchdogProc, nullptr, 0, NULL);

    // Tum thread'leri periyodik ornekle: PSEMU_THREAD_SAMPLE=<saniye>
    if (const char* tse = std::getenv("PSEMU_THREAD_SAMPLE")) {
        unsigned long secs = std::strtoul(tse, nullptr, 0);
        if (secs > 0) {
            CreateThread(NULL, 0, ThreadSamplerProc,
                         reinterpret_cast<LPVOID>(static_cast<uintptr_t>(secs * 1000)),
                         0, NULL);
            LOG_INFO("[THREAD-ORNEK] ornekleyici acildi (" + std::to_string(secs) + " sn)");
        }
    }

    {
        std::stringstream ss;
        ss << "module_start=0x" << std::hex << entry_point 
           << " | original_e_entry=0x" << original_entry << std::dec;
        LOG_INFO(ss.str());
    }

    LOG_INFO("Oyun thread'i hazirlaniyor...");
    LOG_INFO("[TANI] StartExecution (main/loader) thread TID=" + std::to_string(GetCurrentThreadId()));
    DWORD gameThreadId = 0;
    HANDLE hThread = CreateThread(
        NULL,                   // default security attributes
        0,                      // use default stack size
        ExecutionThread,        // thread function name
        reinterpret_cast<LPVOID>(entry_point), // argument to thread function
        0,                      // use default creation flags
        &gameThreadId);         // returns the thread identifier

    if (hThread == NULL) {
        LOG_ERROR("Oyun Thread'i baslatilamadi!");
        return;
    }

    LOG_INFO("[TANI] Oyun (ExecutionThread) TID=" + std::to_string(gameThreadId));

    // Watchdog'un izleyebilmesi icin BAGIMSIZ bir handle kopyasi: asagidaki
    // WaitForSingleObject/CloseHandle bunu kapatmasin.
    DuplicateHandle(GetCurrentProcess(), hThread, GetCurrentProcess(), &g_exec_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);

    // Emulator ana dongusu bitmemesi icin thread'in bitmesini bekle
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    RemoveVectoredExceptionHandler(veh_handle);
}

extern "C" void PsemuNotifyKytyFlip() {}







