#include "kernel/semaphore.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include <cstdlib>
namespace Libs::PsemuSemaphore {
LIB_VERSION("libkernel",1,"libkernel",1,1);
// Core has always supported unnamed private semaphores. The Kyty primitive
// uses this name only as a debug label; every call still creates a real object.
static int KYTY_SYSV_ABI Create(LibKernel::Semaphore::KernelSema* sem,const char* name,
                               uint32_t attr,int initial,int maximum,void* options) {
    return LibKernel::Semaphore::KernelCreateSema(sem,name?name:"",attr,initial,maximum,options);
}
LIB_DEFINE(Register) {
    LIB_FUNC("188x57JYp0g",Create);
    LIB_FUNC("R1Jvn8bSCW8",LibKernel::Semaphore::KernelDeleteSema);
    LIB_FUNC("Zxa0VhQVTsk",LibKernel::Semaphore::KernelWaitSema);
    LIB_FUNC("12wOHk8ywb0",LibKernel::Semaphore::KernelPollSema);
    LIB_FUNC("4czppHBiriw",LibKernel::Semaphore::KernelSignalSema);
    LIB_FUNC("4DM06U2BNEY",LibKernel::Semaphore::KernelCancelSema);
}
}
extern "C" void PsemuRegisterNativeSemaphores(Loader::SymbolDatabase* db) {
    const char* enabled=std::getenv("PSEMU_NATIVE_SEMA");
    if(enabled && enabled[0]=='1')Libs::PsemuSemaphore::Register(db);
}
