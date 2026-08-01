// ============================================================================
// psemu - sceFiber* (Sony kullanici seviyesi coroutine / is sistemi)
// ----------------------------------------------------------------------------
// NEDEN GEREKLI: Astro Bot'un is sistemi FIBER tabanli. psemu'da fiber
// destegi HIC YOKTU; besi de zincirin sonundaki varsayilan stub'a dusup
// RAX=0 donuyordu - yani sceFiberRun "BASARILI" gorunuyor ama hicbir is
// calismiyordu. Olculen sonuc:
//     sceKernelCreateSema    8 cagri
//     sceKernelWaitSema      binlerce
//     sceKernelSignalSema    0 CAGRI          <- uretici taraf hic calismiyor
//     [SEMA] zaman asimi orani 1500/1500 (%100)
// 8+ isci thread'i sonsuza kadar semaforda park ediyordu. Isciye birakilan
// kaynaklar - 0x29 formatli 1920x1080 derinlik hedefi dahil - hic
// olusmuyordu ve oyun "Assertion failed: depthTarget != nullptr" ile
// oluyordu.
//
// UYGULAMA: Windows'un yerlesik fiber API'si (ConvertThreadToFiber /
// CreateFiber / SwitchToFiber) Sony'nin semantigine dogrudan karsilik
// geliyor. Tek dikkat noktasi ABI: misafir giris noktasi SysV, Windows
// fiber proc'u ise Win64. Arada clang'in sysv_abi niteligiyle kopru
// kuruyoruz (qsort karsilastiricisinda ise yarayan ayni desen).
//
// KACIS KAPISI: PSEMU_NO_FIBER=1 -> eski davranis (hepsi stub). Yeni bir
// alt sistem ekliyoruz; tek degiskenle geri alabilmek sart.
// ============================================================================
#ifndef PSEMU_FIBER_H
#define PSEMU_FIBER_H

#include <cstddef>
#include <cstdint>

namespace Psemu::Fiber {

// Sony hata kodlari (yalnizca kullandiklarimiz).
constexpr int kOk                 = 0;
constexpr int kErrorNull          = static_cast<int>(0x80590001); // SCE_FIBER_ERROR_NULL
constexpr int kErrorInvalid       = static_cast<int>(0x80590003); // SCE_FIBER_ERROR_INVALID
constexpr int kErrorState         = static_cast<int>(0x80590006); // SCE_FIBER_ERROR_STATE

bool Enabled();

// _sceFiberInitializeImpl(fiber, name, entry, argOnInitialize,
//                         addrContext, sizeContext, optParam, buildVersion)
int Initialize(void* fiber, const char* name, uint64_t entry, uint64_t arg_on_initialize,
               void* addr_context, size_t size_context);

// sceFiberRun(fiber, argOnRunTo, uint64_t* argOnReturn)
int Run(void* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_return);

// sceFiberSwitch(fiber, argOnRunTo, uint64_t* argOnRun)
int Switch(void* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_run);

// sceFiberReturnToThread(argOnReturn, uint64_t* argOnRun)
int ReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run);

// sceFiberFinalize(fiber)
int Finalize(void* fiber);

} // namespace Psemu::Fiber

#endif // PSEMU_FIBER_H
