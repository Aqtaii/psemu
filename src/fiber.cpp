// ============================================================================
// psemu - sceFiber* uygulamasi (Windows fiber'lari uzerinde)
// ----------------------------------------------------------------------------
// Gerekce ve olcumler icin bkz. include/fiber.h.
//
// SONY SEMANTIGI (SDK belgelerinden ve cagri kaliplarindan):
//   sceFiberInitialize(fiber, name, entry, argOnInitialize, ctx, ctxSize, opt)
//       entry imzasi: void entry(uint64_t argOnInitialize, uint64_t argOnRun)
//   sceFiberRun(fiber, argOnRunTo, &argOnReturn)
//       THREAD baglamindan fiber'a gecer; fiber ReturnToThread cagirinca doner
//   sceFiberSwitch(fiber, argOnRunTo, &argOnRun)
//       FIBER baglamindan baska bir fiber'a gecer
//   sceFiberReturnToThread(argOnReturn, &argOnRun)
//       fiber'dan onu calistiran thread'e doner
//
// WINDOWS KARSILIGI: ConvertThreadToFiber (thread'i fiber yap) +
// CreateFiber + SwitchToFiber. Semantik birebir ortusuyor.
//
// BILINEN SINIRLAMA (bilerek): Sony fiber'in yigini olarak oyunun verdigi
// addrContext/sizeContext'i kullanir; Windows CreateFiber ise KENDI yiginini
// ayirir. Yani misafirin verdigi baglam bellegi yigin olarak KULLANILMIYOR.
// Oyun o bellegi yalnizca yigin alani olarak veriyorsa sorun olmaz; icine
// kendi verisini koyup fiber icinden okuyorsa sorun cikar. Bunu simdilik
// kabul ediyoruz - once is sisteminin CALISMASI gerekiyor; sorun cikarsa
// olcup duzeltiriz. Yigin boyutunu yine de misafirin istediginden aliyoruz.
// ============================================================================
#include "fiber.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Psemu::Fiber {
namespace {

// Misafir giris noktasi SysV ABI ile cagrilmali (clang-cl niteligi).
using GuestFiberEntry = void(__attribute__((sysv_abi)) *)(uint64_t, uint64_t);

struct GuestFiber {
    void*    win_fiber        = nullptr; // Windows fiber (ilk Run'da yaratilir)
    uint64_t entry            = 0;
    uint64_t arg_on_initialize = 0;
    uint64_t arg_on_run       = 0;  // entry'ye / Switch sonrasi verilecek
    uint64_t arg_on_return    = 0;  // ReturnToThread'in verdigi
    void*    resume_to        = nullptr; // ReturnToThread'in donecegi fiber
    size_t   stack_size       = 0;
    bool     started          = false;
};

std::mutex                                  g_mutex;
std::unordered_map<void*, GuestFiber*>      g_fibers;   // misafir SceFiber* -> bizimki
std::atomic<bool>                           g_disabled{false};
std::atomic<bool>                           g_checked{false};

// Bu thread fiber'a cevrildi mi ve hangi fiber'dayiz?
thread_local void*       t_thread_fiber  = nullptr; // ConvertThreadToFiber sonucu
thread_local GuestFiber* t_current       = nullptr; // su an calisan misafir fiber

GuestFiber* Lookup(void* guest) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_fibers.find(guest);
    return it == g_fibers.end() ? nullptr : it->second;
}

// Cagiran thread'i fiber'a cevir (bir kez). SwitchToFiber ancak fiber
// baglamindan cagrilabilir.
bool EnsureThreadIsFiber() {
    if (t_thread_fiber != nullptr) return true;
    void* f = ConvertThreadToFiber(nullptr);
    if (f == nullptr) {
        // Zaten fiber ise GetCurrentFiber gecerli bir deger verir.
        f = GetCurrentFiber();
        if (f == nullptr || f == reinterpret_cast<void*>(0x1e00)) return false;
    }
    t_thread_fiber = f;
    return true;
}

void WINAPI FiberProc(void* param) {
    auto* gf = static_cast<GuestFiber*>(param);
    t_current = gf;
    auto entry = reinterpret_cast<GuestFiberEntry>(gf->entry);
    if (entry != nullptr) {
        entry(gf->arg_on_initialize, gf->arg_on_run);
    }
    // Sony'de giris noktasi DONMEZ (ReturnToThread ile cikar). Yine de
    // donerse surecin cokmemesi icin cagirana geri geciyoruz.
    static std::atomic<int> s_ret{0};
    if (s_ret.fetch_add(1, std::memory_order_relaxed) < 4) {
        printf("[FIBER] UYARI: giris noktasi dondu (Sony'de donmemeli)\n");
        fflush(stdout);
    }
    void* back = gf->resume_to != nullptr ? gf->resume_to : t_thread_fiber;
    t_current = nullptr;
    if (back != nullptr) SwitchToFiber(back);
}

} // namespace

bool Enabled() {
    if (!g_checked.exchange(true, std::memory_order_relaxed)) {
        const char* off = getenv("PSEMU_NO_FIBER");
        if (off != nullptr && off[0] == '1') {
            g_disabled.store(true, std::memory_order_relaxed);
            printf("[FIBER] PSEMU_NO_FIBER=1 - fiber destegi KAPALI (eski stub davranisi)\n");
            fflush(stdout);
        }
    }
    return !g_disabled.load(std::memory_order_relaxed);
}

int Initialize(void* fiber, const char* name, uint64_t entry, uint64_t arg_on_initialize,
               void* addr_context, size_t size_context) {
    if (fiber == nullptr || entry == 0) return kErrorNull;

    auto* gf = new GuestFiber();
    gf->entry             = entry;
    gf->arg_on_initialize = arg_on_initialize;
    // Yigin boyutu: misafirin istedigi kadar, ama makul bir alt sinir koy.
    gf->stack_size = (size_context >= 0x4000 && size_context <= 0x400000)
                         ? size_context
                         : 0x20000; // 128 KB varsayilan
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_fibers.find(fiber);
        if (it != g_fibers.end()) {
            // Ayni SceFiber* yeniden ilklendiriliyor: eskisini birak.
            if (it->second->win_fiber != nullptr) DeleteFiber(it->second->win_fiber);
            delete it->second;
            it->second = gf;
        } else {
            g_fibers.emplace(fiber, gf);
        }
    }

    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 8) {
        printf("[FIBER] init \"%s\" entry=0x%llx ctx=%p/%zu -> yigin %zu bayt\n",
               name != nullptr ? name : "(isimsiz)",
               static_cast<unsigned long long>(entry), addr_context, size_context,
               gf->stack_size);
        fflush(stdout);
    }
    return kOk;
}

int Run(void* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_return) {
    GuestFiber* gf = Lookup(fiber);
    if (gf == nullptr) return kErrorNull;
    if (!EnsureThreadIsFiber()) return kErrorState;

    gf->arg_on_run = arg_on_run_to;
    gf->resume_to  = GetCurrentFiber();

    if (gf->win_fiber == nullptr) {
        gf->win_fiber = CreateFiber(gf->stack_size, &FiberProc, gf);
        if (gf->win_fiber == nullptr) return kErrorState;
        gf->started = true;
    }

    GuestFiber* prev = t_current;
    SwitchToFiber(gf->win_fiber);
    // Buraya fiber ReturnToThread/Switch ile geri dondugunde geliyoruz.
    t_current = prev;
    if (arg_on_return != nullptr) *arg_on_return = gf->arg_on_return;
    return kOk;
}

int Switch(void* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_run) {
    GuestFiber* gf = Lookup(fiber);
    if (gf == nullptr) return kErrorNull;
    if (!EnsureThreadIsFiber()) return kErrorState;

    gf->arg_on_run = arg_on_run_to;
    gf->resume_to  = GetCurrentFiber();

    if (gf->win_fiber == nullptr) {
        gf->win_fiber = CreateFiber(gf->stack_size, &FiberProc, gf);
        if (gf->win_fiber == nullptr) return kErrorState;
        gf->started = true;
    }

    GuestFiber* prev = t_current;
    SwitchToFiber(gf->win_fiber);
    t_current = prev;
    if (arg_on_run != nullptr) *arg_on_run = gf->arg_on_run;
    return kOk;
}

int ReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run) {
    GuestFiber* gf = t_current;
    if (gf == nullptr) return kErrorState; // fiber baglaminda degiliz
    gf->arg_on_return = arg_on_return;

    void* back = gf->resume_to != nullptr ? gf->resume_to : t_thread_fiber;
    if (back == nullptr) return kErrorState;

    t_current = nullptr;
    SwitchToFiber(back);
    // Yeniden calistirildigimizda buradan devam ediyoruz.
    t_current = gf;
    if (arg_on_run != nullptr) *arg_on_run = gf->arg_on_run;
    return kOk;
}

int Finalize(void* fiber) {
    GuestFiber* gf = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_fibers.find(fiber);
        if (it == g_fibers.end()) return kErrorNull;
        gf = it->second;
        g_fibers.erase(it);
    }
    // CALISAN fiber'i silmek surecin yigini altindan halinin cekilmesidir.
    // Sony'de de calisan fiber finalize edilemez; sessizce birakiyoruz.
    if (gf->win_fiber != nullptr && gf != t_current) {
        DeleteFiber(gf->win_fiber);
    }
    delete gf;
    return kOk;
}

} // namespace Psemu::Fiber
