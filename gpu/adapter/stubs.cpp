// ============================================================================
// psemu GPU adapteri — Kyty'nin ihtiyac duydugu ama bizim TAM derlemedigimiz
// kucuk fonksiyonlarin minimal stub'lari. (Link yuzeyini kucultur.)
// Bu fonksiyonlar Kyty'nin kernel'inden (pthread.cpp, fileSystem.cpp) referans
// ediliyor; render yolunda cagrilmadiklari surece dummy donus yeterli. Gercek
// davranis gerekirse ilk-frame debug'inda ele alinir.
// ============================================================================
#include <cstdint>
#include <cstring>

#include <windows.h>
#include <cpuinfo.h>

#include "common/abi.h"           // KYTY_SYSV_ABI
#include "libs/network.h"         // Libs::Network::Net bildirimleri
#include "loader/runtimeLinker.h" // Loader::RuntimeLinker + Program
#include "graphics/presentation/renderDoc.h" // RenderDoc* (renderDoc.cpp derlenmedi)

// --- cpuinfo -----------------------------------------------------------------
// systemInfo.cpp yalnizca CPU paket ADINI istiyor (kozmetik log).
extern "C" bool CPUINFO_ABI cpuinfo_initialize(void) { return true; }

extern "C" const struct cpuinfo_package* CPUINFO_ABI cpuinfo_get_package(uint32_t /*index*/) {
    static struct cpuinfo_package s_pkg = {};
    std::strncpy(s_pkg.name, "psemu Host CPU", CPUINFO_PACKAGE_NAME_MAX - 1);
    return &s_pkg;
}

// --- Network (fileSystem.cpp -> soket fd'lerine yazma; bizde ag yok) ---------
namespace Libs::Network::Net {
int64_t KYTY_SYSV_ABI Send(int /*s*/, const void* /*buf*/, uint64_t /*len*/, int /*flags*/) { return -1; }
int64_t KYTY_SYSV_ABI Recv(int /*s*/, void* /*buf*/, uint64_t /*len*/, int /*flags*/) { return -1; }
bool    KYTY_SYSV_ABI IsSocket(int /*s*/) { return false; }
int     KYTY_SYSV_ABI SocketClose(int /*s*/) { return 0; }
} // namespace Libs::Network::Net

// --- Posix::GetErrorAddr (errno adresi) -------------------------------------
namespace Libs::Posix {
int* KYTY_SYSV_ABI GetErrorAddr() {
    static thread_local int s_errno = 0;
    return &s_errno;
}
} // namespace Libs::Posix

// --- LibKernel: bekleyen sinyal gonderimi (bizde sinyal yok) ----------------
namespace Libs::LibKernel {
void KYTY_SYSV_ABI KernelDispatchPendingSignalForCurrentThread() {}
} // namespace Libs::LibKernel

// --- RenderDoc (debug capture araci — bizde yok, hepsi no-op) ----------------
namespace Libs::Graphics {
void RenderDocInit() {}
bool RenderDocIsLoaded() { return false; }
void RenderDocSetActiveWindow(vk::Instance /*instance*/, HWND /*window*/) {}
void RenderDocRequestCapture() {}
void RenderDocOnPresent() {}
} // namespace Libs::Graphics

// --- Loader::RuntimeLinker (pthread.cpp TLS icin singleton kullaniyor) -------
// psemu kendi loader'ini kullaniyor; Kyty'nin runtimeLinker.cpp'sini
// derlemiyoruz. pthread.cpp yalnizca FindProgramByAddr + DeleteTlss cagiriyor.
namespace Loader {
RuntimeLinker::RuntimeLinker() = default;
RuntimeLinker::~RuntimeLinker() = default;
Program* RuntimeLinker::FindProgramByAddr(uint64_t /*vaddr*/) { return nullptr; }
void     RuntimeLinker::DeleteTlss(int /*thread_id*/) {}
} // namespace Loader
