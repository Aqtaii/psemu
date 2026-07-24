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
#include <mutex>    // direct memory havuzu kilidi
#include "nids.h"
#include "video.h"
#include "kernel/eventQueue.h"
#include "graphics/presentation/videoOut.h"
#include <immintrin.h>

extern "C" void PsemuMarkCpuModified(uint64_t vaddr, uint64_t size);

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
static bool SafeReadable(const void* p, size_t n) {
    if (p == nullptr || n == 0) return false;
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(p);
    const uint8_t* end = cur + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
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
static std::string TranslateGuestPath(const std::string& guest) {
    if (guest.empty()) return guest;
    if (guest.rfind("/app0/", 0) == 0)  return g_game_root + "/" + guest.substr(6);
    if (guest == "/app0")               return g_game_root;
    if (guest.rfind("/hostapp/", 0) == 0) return g_game_root + "/" + guest.substr(9);
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
static const uint64_t  kDmemSize   = 0x100000000ULL; // 4 GB adres alani rezervi

// AGC (GPU) modulunun descriptor'lardaki FIZIKSEL adresleri CPU'ya cevirebilmesi
// icin havuz tabanini disari veriyoruz (CPU = g_dmem_base_addr + phys).
uint64_t g_dmem_base_addr = 0;

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

static GuestMutex* GetOrCreateMutex(uint64_t* slot) {
    if (slot == nullptr || !SafeWritable(slot, sizeof(uint64_t))) return nullptr;
    std::lock_guard<std::mutex> lk(g_sync_create_mutex);
    uint64_t h = *slot;
    if (h != 0 && g_known_mutexes.count(h) != 0) {
        return reinterpret_cast<GuestMutex*>(h);
    }
    GuestMutex* m = new GuestMutex();
    InitializeCriticalSection(&m->cs);
    *slot = reinterpret_cast<uint64_t>(m);
    g_known_mutexes.insert(*slot);
    return m;
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
    for (const C& c : cands) {
        if (c.d == nullptr || c.n == 0 || c.n > 65536) continue;
        if (!Readable(c.d, c.n * 2)) continue;

        bool fixed = false;
        // Fix the string in-place
        for (size_t i = 0; i < c.n; i++) {
            if (c.d[i] >= 0xD800 && c.d[i] <= 0xDBFF) {
                // High surrogate, needs a low surrogate after it
                if (i + 1 >= c.n || !(c.d[i + 1] >= 0xDC00 && c.d[i + 1] <= 0xDFFF)) {
                    uint16_t* mutable_d = const_cast<uint16_t*>(c.d);
                    mutable_d[i] = 0x003F; // Replace with '?'
                    fixed = true;
                }
            } else if (c.d[i] >= 0xDC00 && c.d[i] <= 0xDFFF) {
                // Low surrogate without preceding high surrogate
                if (i == 0 || !(c.d[i - 1] >= 0xD800 && c.d[i - 1] <= 0xDBFF)) {
                    uint16_t* mutable_d = const_cast<uint16_t*>(c.d);
                    mutable_d[i] = 0x003F; // Replace with '?'
                    fixed = true;
                }
            }
        }

        if (fixed) {
            static volatile LONG s_n = 0;
            if (InterlockedIncrement(&s_n) <= 50) {
                std::cout << "[UTF16-FIX] Sabitlenmis bozuk UTF-16 dizisi: len=" << c.n << " layout=" << c.how << std::endl;
            }
            return; // Found and fixed the layout, stop checking other layouts
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

// Bu thread'in tp'si; ilk fs: erisiminde olusturulur.
static uint64_t GetThreadTlsBase() {
    static thread_local uint64_t t_tp = 0;
    if (t_tp == 0) {
        t_tp = CreateTlsBlockForCurrentThread();
        if (t_tp != 0) {
            if (g_tls_base == 0) g_tls_base = t_tp; // ilk blok: geriye uyumluluk
            __try {
                SafeWriteFsBase(t_tp);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // FSGSBASE donanim/IS tarafindan desteklenmiyorsa VEH handler devrede kalir
            }
            static volatile LONG s_n = 0;
            LONG n = InterlockedIncrement(&s_n);
            if (n <= 8) {
                printf("[TLS] Thread'e ozel TLS blogu #%ld: TID=%lu tp=0x%llx (HW FS_BASE ayarlandi)\n",
                       n, GetCurrentThreadId(), t_tp);
                fflush(stdout);
            }
        }
    }
    return t_tp;
}

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

static DWORD WINAPI HangWatchdogProc(LPVOID) {
    // Cok agresif: worker sadece ~300ms sessiz kalirsa bile ornek al.
    // Sessiz olum (stack overflow) ihtimaline karsi hizli sampling gerekiyor.
    for (;;) {
        Sleep(150);
        if (g_worker_thread == nullptr || g_last_activity == 0) continue;
        if (g_watchdog_dumps >= kWatchdogMaxDumps) continue;

        ULONGLONG now = GetTickCount64();
        if (now - g_last_activity < 300) continue; // hala aktif

        if (SuspendThread(g_worker_thread) == (DWORD)-1) continue;

        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_worker_thread, &c)) {
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
        ResumeThread(g_worker_thread);
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

    // Bazi derleyiciler hizalama/redundant amaçlarla birden fazla 66 on eki uretebilir
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

LONG WINAPI Core::SyscallExceptionFilter(EXCEPTION_POINTERS* ExceptionInfo) {
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
    // veya YAZMA) cokerdi. Onceki cozum her taşma komutunu tek tek atliyordu
    // (whack-a-mole; her timing degisiminde yeni RVA — Vulkan wiring sonrasi
    // 0x2e08aa gibi). GENEL COZUM: bu fonksiyonda (0x2dfff0..0x2e0900)
    // NULL-base fault olunca, NULL register'i (rbx/rdx) sifirlanmis bir DUMMY
    // buffer'a yonlendirip komutu YENIDEN CALISTIR. Boylece tum taşma erisimleri
    // (5. kayit) zararsizca dummy'ye gider, ilk 4 gecerli kayit tabloda kalir,
    // fonksiyon normal tamamlanir. Instruction atlama / whack-a-mole YOK.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
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

    // Texture metadata read recovery at RVA 0x1654f8 (mov esi, [rcx + 0x120])
    if (code == EXCEPTION_ACCESS_VIOLATION && ctx->Rip == g_base_addr + 0x1654f8) {
        ctx->Rsi = 1;
        ctx->Rip = g_base_addr + 0x165508; // Jump past jz check to force main menu scene transition!
        LOG_INFO("[VEH-RECOVER] Force-completed texture load at RVA 0x1654f8 -> transitioning scene to main menu!");
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Eger Syscall disinda baska bir cokme yasandiysa detayli register dokumu yap
    std::stringstream ss;
    ss << "CRASH yakalandi! Kod: 0x" << std::hex << code
       << " | RIP: 0x" << ctx->Rip;
    if (ctx->Rip >= g_base_addr && ctx->Rip < g_base_addr + g_module_size) {
        ss << " (RVA 0x" << (ctx->Rip - g_base_addr) << ")";
    }
    if (code == 0xC00000FD) {
        ss << " [STACK OVERFLOW]";
    }

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
                uint64_t value  = tls_tp + info.disp;

                // Gurultu filtresi: her fs: erisim adresini sadece birkac kez logla
                {
                    static std::map<uint64_t, int> s_tls_seen;
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
            uint32_t plt_index = static_cast<uint32_t>(access_addr - 0x10000000000ULL);
            std::string func_name = "UNKNOWN_PLT_" + std::to_string(plt_index);
            std::string readable_name = "";
            if (g_plt_names.find(plt_index) != g_plt_names.end()) {
                func_name = g_plt_names[plt_index];
                if (g_nid_to_name.find(func_name) != g_nid_to_name.end()) {
                    readable_name = g_nid_to_name[func_name];
                }
            }
            
            // Gurultu filtresi: her fonksiyonu ilk N kez logla (asagidaki
            // RET-simulasyon logu da ayni karara bagli).
            bool log_this = ShouldLogPlt(plt_index,
                                         readable_name.empty() ? func_name : readable_name);

            if (log_this) {
                std::stringstream hle_ss;
                hle_ss << "[PRX-HLE] " << func_name;
                if (!readable_name.empty()) {
                    hle_ss << " (" << readable_name << ")";
                }
                hle_ss << " (PLT#" << plt_index << ")"
                       << " | TID=" << std::dec << GetCurrentThreadId() << std::hex
                       << " RDI=0x" << ctx->Rdi
                       << " RSI=0x" << ctx->Rsi
                       << " RDX=0x" << ctx->Rdx
                       << " RCX=0x" << ctx->Rcx
                       << " R8=0x"  << ctx->R8
                       << " R9=0x"  << ctx->R9
                       << " RSP=0x" << ctx->Rsp
                       << std::dec;
                LOG_INFO(hle_ss.str());
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
            if (Agc::Dispatch(func_name, readable_name, ctx)) {
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
            } else if (readable_name == "sceKernelMapDirectMemory") {
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
            else if (readable_name == "malloc") {
                // malloc(size): RDI=size
                size_t size = static_cast<size_t>(ctx->Rdi);
                size_t alloc_sz = size ? (size + 65536) : 65536;
                void* p = _aligned_malloc(alloc_sz, 16);
                if (p) memset(p, 0, alloc_sz);
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
                    if (old_p && SafeReadable(old_p, 1)) {
                        size_t copy_len = (size > 0) ? size : 16;
                        if (SafeReadable(old_p, copy_len)) {
                            memcpy(p, old_p, copy_len);
                        }
                    }
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
            } else if (readable_name == "memcmp") {
                const void* a = reinterpret_cast<const void*>(ctx->Rdi);
                const void* b = reinterpret_cast<const void*>(ctx->Rsi);
                size_t n = static_cast<size_t>(ctx->Rdx);
                int r = 0;
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
                // nlohmann basic_json yerlesimi: +0 tip baytı, +8 deger/pointer.
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
                            if (ret == g_base_addr + 0x1e3cf7) {
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
            } else if (func_name.find("fnUEjBCNRVU") != std::string::npos) {
                // wmemchr (PS5 wchar_t is 32-bit or 16-bit?)
                uint16_t* s16 = (uint16_t*)ctx->Rdi;
                uint32_t* s32 = (uint32_t*)ctx->Rdi;
                uint32_t c = (uint32_t)ctx->Rsi;
                size_t n = (size_t)ctx->Rdx;
                uint64_t res = 0;
                
                std::stringstream dump;
                dump << "[wmemchr] RDI=0x" << std::hex << ctx->Rdi << " c=" << c << " n=" << std::dec << n << " | ";
                if (s16 && SafeReadable(s16, n * 2)) {
                    for(size_t i=0; i<min(n, (size_t)8); i++) dump << std::hex << s16[i] << " ";
                }
                LOG_INFO(dump.str());

                if (s32 && n > 0 && SafeReadable(s32, n * 4)) {
                    for (size_t i = 0; i < n; i++) {
                        if (s32[i] == c) {
                            res = (uint64_t)&s32[i];
                            break;
                        }
                    }
                }
                // Try 16-bit fallback if 32-bit didn't find it
                if (!res && s16 && n > 0 && SafeReadable(s16, n * 2)) {
                    for (size_t i = 0; i < n; i++) {
                        if (s16[i] == (uint16_t)c) {
                            res = (uint64_t)&s16[i];
                            LOG_INFO("[wmemchr] Found using 16-bit search!");
                            break;
                        }
                    }
                }
                ctx->Rax = res;
                special_return_set = true;
            } else if (func_name.find("Noj9PsJrsa8") != std::string::npos) {
                // wmemmove (PS5 wchar_t is 32-bit)
                uint32_t* dest = (uint32_t*)ctx->Rdi;
                const uint32_t* src = (const uint32_t*)ctx->Rsi;
                size_t n = (size_t)ctx->Rdx;
                if (dest && src && n > 0 && SafeReadable(src, n * 4) && SafeWritable(dest, n * 4)) {
                    memmove(dest, src, n * 4);
                }
                ctx->Rax = (uint64_t)dest;
                special_return_set = true;
            } else if (func_name.find("kALvdgEv5ME") != std::string::npos || func_name.find("9nf8joUTSaQ") != std::string::npos || func_name.find("rcQCUr0EaRU") != std::string::npos || func_name.find("sUP1hBaouOw") != std::string::npos || func_name.find("p6LrHjIQMdk") != std::string::npos || func_name.find("hqi8yMOCmG0") != std::string::npos || func_name.find("QW2jL1J5rwY") != std::string::npos || func_name.find("P8F2oavZXtY") != std::string::npos || func_name.find("Q1BL70XVV0o") != std::string::npos) {
                // _Locksyslock / _Unlocksyslock / locales / exceptions
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
                // int sceKernelGetProcessTime(uint64_t* time_us): RDI=time_us pointer
                uint64_t us = MonotonicNs() / 1000ull;
                uint64_t* out = reinterpret_cast<uint64_t*>(ctx->Rdi);
                if (out != nullptr && SafeWritable(out, 8)) {
                    *out = us;
                }
                ctx->Rax = 0; // 0 = SUCCESS
                special_return_set = true;
            } else if (readable_name == "sceKernelGetProcessTimeCounter") {
                // int sceKernelGetProcessTimeCounter(uint64_t* counter): RDI=counter pointer
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                uint64_t* out = reinterpret_cast<uint64_t*>(ctx->Rdi);
                if (out != nullptr && SafeWritable(out, 8)) {
                    *out = static_cast<uint64_t>(now.QuadPart);
                }
                ctx->Rax = 0; // 0 = SUCCESS
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
                if (res_ptr && SafeWritable(res_ptr, 64)) {
                    char* mount_str = reinterpret_cast<char*>(res_ptr);
                    strncpy(mount_str, "/saveData0", 32);
                }
                ctx->Rax = 0;
                special_return_set = true;
                LOG_INFO("[SaveData] SaveDataMount3 -> SUCCESS (0) [/saveData0]");
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
                if (!host.empty()) {
                    f = fopen(host.c_str(), "rb");
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
                    if (s_n < 20 || total > 65536) {
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
            } else if (readable_name == "PadOpen" || readable_name == "PadGetHandle") {
                ctx->Rax = 1; // gecerli handle (0 DEGIL - kritik)
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
                // (cond, mutex[, usec]) - mutex kilitli gelir, kilitli doner.
                GuestCond*  c = GetOrCreateCond(reinterpret_cast<uint64_t*>(ctx->Rdi));
                GuestMutex* m = GetOrCreateMutex(reinterpret_cast<uint64_t*>(ctx->Rsi));
                bool timed = (readable_name == "scePthreadCondTimedwait" ||
                              readable_name == "pthread_cond_timedwait");
                DWORD ms = INFINITE;
                if (timed) {
                    uint64_t usec = static_cast<uint32_t>(ctx->Rdx);
                    uint64_t conv = usec / 1000;
                    ms = (conv >= INFINITE) ? (INFINITE - 1)
                                            : static_cast<DWORD>(conv);
                }
                BOOL ok = TRUE;
                if (c && m) {
                    ok = SleepConditionVariableCS(&c->cv, &m->cs, ms);
                } else {
                    // Tutamac kurulamadi: en azindan bekle, MESGUL DONGU olma.
                    Sleep(timed ? (ms == INFINITE ? 1u : (ms ? ms : 1u)) : 1u);
                    ok = FALSE;
                }
                // Zaman asimi -> ETIMEDOUT(60). Bu ONEMLI: 0 donmek
                // "sinyallendi" demektir ve oyun bos kuyruktan gorev
                // okumaya calisir.
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
            // GRAFIK (GNM/Gen5) BASLATMA - GEÇICI STUB
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
                // RVA 0x29516'da coküyordu.
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
            else if (readable_name == "GraphicsDcbSetCxRegistersIndirect" ||
                     readable_name == "GraphicsDcbSetShRegistersIndirect" ||
                     readable_name == "GraphicsDcbSetUcRegistersIndirect") {
                // (CommandBuffer* buf, const ShaderRegister* regs, uint32_t num_regs)
                uint32_t* cmd = CbAllocateDW(ctx->Rdi, 5); // KytyPS5: AllocateDW(5)
                if (cmd) memset(cmd, 0, 5 * 4);
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
            // AGC (GPU / HLE render) — sceAgc* + Graphics* yuzeyi
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
                    }

                    if (log_this) {
                        printf("[PLT-HOOK] HLE Stub calisti -> Return RAX=0x%llx | RET to: 0x%llx\n", ctx->Rax, ret_addr);
                        fflush(stdout);
                    }

                    // RET komutu simulasyonu
                    ctx->Rip = ret_addr;
                    ctx->Rsp += 8;

                    return EXCEPTION_CONTINUE_EXECUTION;
                }
        }

        // ================================================================
        // Non-PLT EXEC violation handler (Harici kütüphane atlamaları)
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

        ss << " | " << (access_type == 0 ? "READ" : (access_type == 1 ? "WRITE" : "EXEC"))
           << " violation @ 0x" << access_addr;

        // Faulting komutun kendi baytlarini goster - tahmin yerine gercek veriyle
        // ilerleyebilmek icin. RIP oyun modulu icindeyse (yani gercekten calisan
        // bir komuttan kaynaklaniyorsa, sentinel/unmapped bir hedef degilse) anlamli.
        if (IsInModuleRange(ctx->Rip) && SafeReadable(reinterpret_cast<void*>(ctx->Rip), 16)) {
            const uint8_t* rip_bytes = reinterpret_cast<const uint8_t*>(ctx->Rip);
            ss << "\n[-] Faulting komut baytlari @ RIP (RVA: 0x" << (ctx->Rip - g_base_addr) << "): ";
            for (int bi = 0; bi < 16; bi++) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X ", rip_bytes[bi]);
                ss << buf;
            }
        }

        // Stack Dump (READ/WRITE/EXEC farketmeksizin - RVA'yi cozup gercek komutu
        // gormek icin call zincirini takip etmek her turlu ihlalde faydali)
        {
            uint64_t* stack_ptr = reinterpret_cast<uint64_t*>(ctx->Rsp);
            ss << "\n[-] --- STACK DUMP ---";
            for (int i = 0; i < 16; i++) {
                if (SafeReadable(stack_ptr + i, sizeof(uint64_t))) {
                    uint64_t val = stack_ptr[i];
                    ss << "\n    RSP+" << std::hex << (i * 8) << ": 0x" << val;
                    if (IsInTextSegment(val)) {
                        ss << " [<-- VALID CODE OFFSET: 0x" << (val - g_base_addr) << "]";
                    }
                } else {
                    ss << "\n    RSP+" << std::hex << (i * 8) << ": [INACCESSIBLE]";
                    break; // Bellek erisilemez ise asagiya inmeye gerek yok
                }
            }
            ss << "\n[-] ------------------";
        }
    }

    // Genel backtrace (HER cokme tipi icin, ozellikle STACK OVERFLOW):
    // RSP'den yukari tarayip oyun modulune ait donus adreslerini RVA olarak
    // topla. Ayni RVA'nin tekrar tekrar gorunmesi = OZYINELEME (stack overflow).
    {
        uint64_t* sp = reinterpret_cast<uint64_t*>(ctx->Rsp);
        ss << "\n[-] [backtrace RVA]";
        int found = 0;
        for (int i = 0; i < 256 && found < 16; i++) {
            if (!SafeReadable(sp + i, sizeof(uint64_t))) break;
            uint64_t v = sp[i];
            if (v >= g_base_addr && v < g_base_addr + g_module_size) {
                ss << " 0x" << std::hex << (v - g_base_addr);
                found++;
            }
        }
    }

    ss << std::dec;
    LOG_ERROR(ss.str());
    
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
        ss << "[INFO] PT_SCE_PROCPARAM bellek dökümü (0x" << std::hex << target_addr << ") ilk 96 byte:\n";
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
        for (size_t i = 0; i < scan_size - 24; i += 4) {
            if (!SafeReadable(&scan_base[i], 24)) {
                break;
            }
            
            uint32_t val1 = *reinterpret_cast<uint32_t*>(&scan_base[i]);
            if (val1 == 0x6AC156EF) {
                uint32_t val2 = *reinterpret_cast<uint32_t*>(&scan_base[i + 12]);
                if (val2 == 0x6AC15610) {
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
            for (size_t i = 0; i < scan_size - 8; i += 8) {
                if (!SafeReadable(&scan_base[i], 8)) {
                    break;
                }
                
                uint64_t ptr = *reinterpret_cast<uint64_t*>(&scan_base[i]);
                if (ptr == found_block_array) {
                    if (i >= 0x30) {
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
    // uretip VirtualAlloc PAGE_EXECUTE_READWRITE bir bloğa yaziyoruz.
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
    if (g_init_vaddr != 0) {
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

    // Hang watchdog thread'ini baslat (worker takilirsa RIP'ini doksun)
    CreateThread(NULL, 0, HangWatchdogProc, nullptr, 0, NULL);

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

    // Emulator ana dongusu bitmemesi icin thread'in bitmesini bekle
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    RemoveVectoredExceptionHandler(veh_handle);
}

extern "C" void PsemuNotifyKytyFlip() {}

