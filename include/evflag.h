// ============================================================================
// psemu - sceKernel*EventFlag (Sony olay bayraklari)
// ----------------------------------------------------------------------------
// NEDEN GEREKLI: nids.h yedi fonksiyonun da ADINI biliyordu ama core.cpp'de
// HICBIRININ govdesi yoktu; hepsi zincirin sonundaki varsayilan stub'a dusup
// RAX=0 (basarili) donuyordu. Yani:
//   - CreateEventFlag  "basardim" diyor ama ortada bayrak YOK
//   - WaitEventFlag    aninda "kosul saglandi" diyor
//   - SetEventFlag     hicbir seyi uyandirmiyor
// Bu, semafordaki hatanin ayni sinifi: bekleyen thread'e "is geldi" demek.
// [HLE-EKSIK] tanisi sceKernelCreateEventFlag'i tam da ilerledigimiz kod
// sinirinda (RVA 0xdf1433) yakaladi.
//
// Event flag PS5'te temel senkronizasyon primitifi: 64 bitlik bir desen,
// isteyen bitleri AND/OR ile bekliyor, uretici bitleri set ediyor.
//
// KACIS KAPISI: PSEMU_NO_EVFLAG=1 -> eski davranis (hepsi stub, RAX=0).
// ============================================================================
#ifndef PSEMU_EVFLAG_H
#define PSEMU_EVFLAG_H

#include <cstdint>

namespace Psemu::EvFlag {

// Bekleme kipleri (Sony)
constexpr uint32_t kWaitAnd      = 0x01;
constexpr uint32_t kWaitOr       = 0x02;
constexpr uint32_t kWaitClearAll = 0x10;
constexpr uint32_t kWaitClearPat = 0x20;

constexpr int kOk         = 0;
constexpr int kErrTimeout = static_cast<int>(0x8002003C); // ETIMEDOUT
constexpr int kErrInval   = static_cast<int>(0x80020016); // EINVAL
constexpr int kErrCanceled= static_cast<int>(0x8002005E); // ECANCELED

bool Enabled();

// sceKernelCreateEventFlag(out, name, attr, initPattern, optParam)
int Create(uint64_t* out_handle, const char* name, uint32_t attr, uint64_t init_pattern);

// sceKernelDeleteEventFlag(ef)
int Delete(uint64_t handle);

// sceKernelSetEventFlag(ef, bitPattern)   -> desen |= bits
int Set(uint64_t handle, uint64_t bits);

// sceKernelClearEventFlag(ef, bitPattern) -> desen &= bits (Sony: TUTULACAK bitler)
int Clear(uint64_t handle, uint64_t bits);

// sceKernelPollEventFlag(ef, bitPattern, waitMode, pResultPat)
int Poll(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result);

// sceKernelWaitEventFlag(ef, bitPattern, waitMode, pResultPat, pTimeoutUs)
// timeout_us: nullptr ise sonsuz istenmis demektir (bkz. .cpp'deki ust sinir).
int Wait(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result,
         const uint32_t* timeout_us);

// sceKernelCancelEventFlag(ef, setPattern, pNumWaitThreads)
int Cancel(uint64_t handle, uint64_t set_pattern, int* num_waiting);

} // namespace Psemu::EvFlag

#endif // PSEMU_EVFLAG_H
