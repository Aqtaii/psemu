#include "kernel/pthread.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include <cstdlib>
namespace Libs::PsemuClock {
LIB_VERSION("libkernel",1,"libkernel",1,1);
LIB_DEFINE(Register) {
    LIB_FUNC("-2IRUCO--PM",LibKernel::KernelReadTsc);
    LIB_FUNC("1j3S3n-tTW4",LibKernel::KernelGetTscFrequency);
    LIB_FUNC("4J2sUJmuHZQ",LibKernel::KernelGetProcessTime);
    LIB_FUNC("fgxnMeTNUtY",LibKernel::KernelGetProcessTimeCounter);
    LIB_FUNC("BNowx2l588E",LibKernel::KernelGetProcessTimeCounterFrequency);
}
}
extern "C" void PsemuRegisterNativeClocks(Loader::SymbolDatabase* db) {
    // VideoOut uses these exact native functions for its status timestamps.
    // Never mix those values with the old QPC-domain guest counter imports.
    const char* enabled=std::getenv("PSEMU_NATIVE_CLOCK");
    if(enabled && enabled[0]=='1')Libs::PsemuClock::Register(db);
}
