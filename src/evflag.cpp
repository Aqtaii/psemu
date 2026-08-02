// ============================================================================
// psemu - sceKernel*EventFlag uygulamasi
// ----------------------------------------------------------------------------
// Gerekce icin bkz. include/evflag.h.
//
// TASARIM: her bayrak bir mutex + condition_variable + 64 bitlik desen.
// Tutamac olarak nesnenin kendi adresini veriyoruz; misafir bunu opak bir
// sayi olarak tasiyor. Silinen bayraclari kayittan cikariyoruz ama BELLEGI
// BIRAKMIYORUZ: baska bir thread hala o bayragi bekliyor olabilir ve altindan
// nesneyi cekmek cokme demek (semafor/mutex tarafinda da ayni yaklasim).
//
// SONSUZ BEKLEME: misafir zaman asimi vermezse Sony sonsuz bekler. Biz de
// bekliyoruz ama kilitlenmeyi TANI EDILEBILIR tutmak icin ust sinir koyduk
// (PSEMU_EVFLAG_WAIT_MS, varsayilan 5000). Sinir dolarsa ETIMEDOUT donuyoruz -
// "sahte basari" donmek yerine, cunku sahte basari bekleyeni hazir olmayan
// veriyle ilerletiyor (semafordaki hatanin ta kendisi).
// ============================================================================
#include "evflag.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

namespace Psemu::EvFlag {
namespace {

struct Flag {
    std::mutex              m;
    std::condition_variable cv;
    uint64_t                pattern  = 0;
    int                     waiters  = 0;
    bool                    canceled = false;
    bool                    deleted  = false;
    char                    name[32] = {};
};

std::mutex          g_reg_mutex;
std::set<Flag*>     g_flags;
std::atomic<bool>   g_disabled{false};
std::atomic<bool>   g_checked{false};

Flag* Lookup(uint64_t handle) {
    auto* f = reinterpret_cast<Flag*>(handle);
    if (f == nullptr) return nullptr;
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    return g_flags.count(f) != 0 ? f : nullptr;
}

unsigned WaitCapMs() {
    static const unsigned s_cap = [] {
        const char* e = std::getenv("PSEMU_EVFLAG_WAIT_MS");
        int v = (e != nullptr) ? atoi(e) : 5000;
        return static_cast<unsigned>(v > 0 ? v : 5000);
    }();
    return s_cap;
}

// Kosul saglandi mi? AND: tum bitler, OR: en az bir bit.
bool Satisfied(uint64_t pattern, uint64_t bits, uint32_t mode) {
    if (bits == 0) return true;
    return (mode & kWaitOr) != 0 ? (pattern & bits) != 0
                                 : (pattern & bits) == bits;
}

// Kosul saglandiktan SONRA istenen temizligi uygula.
void ApplyClear(Flag* f, uint64_t bits, uint32_t mode) {
    if ((mode & kWaitClearAll) != 0) f->pattern = 0;
    else if ((mode & kWaitClearPat) != 0) f->pattern &= ~bits;
}

} // namespace

bool Enabled() {
    if (!g_checked.exchange(true, std::memory_order_relaxed)) {
        const char* off = getenv("PSEMU_NO_EVFLAG");
        if (off != nullptr && off[0] == '1') {
            g_disabled.store(true, std::memory_order_relaxed);
            printf("[EVFLAG] PSEMU_NO_EVFLAG=1 - olay bayraklari KAPALI (eski stub davranisi)\n");
            fflush(stdout);
        }
    }
    return !g_disabled.load(std::memory_order_relaxed);
}

int Create(uint64_t* out_handle, const char* name, uint32_t /*attr*/, uint64_t init_pattern) {
    if (out_handle == nullptr) return kErrInval;
    auto* f = new Flag();
    f->pattern = init_pattern;
    if (name != nullptr) {
        strncpy(f->name, name, sizeof(f->name) - 1);
    }
    {
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        g_flags.insert(f);
    }
    *out_handle = reinterpret_cast<uint64_t>(f);

    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 8) {
        printf("[EVFLAG] create \"%s\" desen=0x%llx -> tutamac=%p\n",
               f->name, static_cast<unsigned long long>(init_pattern), (void*)f);
        fflush(stdout);
    }
    return kOk;
}

int Delete(uint64_t handle) {
    Flag* f = Lookup(handle);
    if (f == nullptr) return kErrInval;
    {
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        g_flags.erase(f);
    }
    {
        std::lock_guard<std::mutex> lk(f->m);
        f->deleted = true;
    }
    f->cv.notify_all(); // bekleyenler uyansin, yoksa sonsuza kadar asili kalirlar
    // Nesneyi BILEREK sizdiriyoruz: hala bekleyen olabilir (bkz. dosya basi).
    return kOk;
}

int Set(uint64_t handle, uint64_t bits) {
    Flag* f = Lookup(handle);
    if (f == nullptr) return kErrInval;
    {
        std::lock_guard<std::mutex> lk(f->m);
        f->pattern |= bits;
    }
    f->cv.notify_all();
    return kOk;
}

int Clear(uint64_t handle, uint64_t bits) {
    Flag* f = Lookup(handle);
    if (f == nullptr) return kErrInval;
    std::lock_guard<std::mutex> lk(f->m);
    f->pattern &= bits; // Sony: argumandaki bitler TUTULUR, digerleri silinir
    return kOk;
}

int Poll(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result) {
    Flag* f = Lookup(handle);
    if (f == nullptr) return kErrInval;
    std::lock_guard<std::mutex> lk(f->m);
    if (!Satisfied(f->pattern, bits, mode)) {
        if (result != nullptr) *result = f->pattern;
        return kErrTimeout; // Sony pollda da ETIMEDOUT doner
    }
    if (result != nullptr) *result = f->pattern;
    ApplyClear(f, bits, mode);
    return kOk;
}

int Wait(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result,
         const uint32_t* timeout_us) {
    Flag* f = Lookup(handle);
    if (f == nullptr) return kErrInval;

    unsigned ms = WaitCapMs();
    if (timeout_us != nullptr) {
        const unsigned want = *timeout_us / 1000u;
        if (want < ms) ms = want;
    }

    std::unique_lock<std::mutex> lk(f->m);
    f->waiters++;
    const bool ok = f->cv.wait_for(
        lk, std::chrono::milliseconds(ms),
        [&] { return f->deleted || f->canceled || Satisfied(f->pattern, bits, mode); });
    f->waiters--;

    if (result != nullptr) *result = f->pattern;
    if (f->deleted || f->canceled) return kErrCanceled;
    if (!ok) return kErrTimeout;
    ApplyClear(f, bits, mode);
    return kOk;
}

int Cancel(uint64_t handle, uint64_t set_pattern, int* num_waiting) {
    Flag* f = Lookup(handle);
    if (f == nullptr) return kErrInval;
    {
        std::lock_guard<std::mutex> lk(f->m);
        if (num_waiting != nullptr) *num_waiting = f->waiters;
        f->pattern  = set_pattern;
        f->canceled = true;
    }
    f->cv.notify_all();
    {   // Cancel tek seferlik: bayrak sonrasinda yeniden kullanilabilir olmali
        std::lock_guard<std::mutex> lk(f->m);
        f->canceled = false;
    }
    return kOk;
}

} // namespace Psemu::EvFlag
