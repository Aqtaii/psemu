#include "kernel/semaphore.h"
#include "libs/errno.h"
#include <windows.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
extern "C" void* PsemuKytyGetFunc(const char* nid);
#define CHECK(x) do {if(!(x)){std::printf("[SEMAPHORE-TEST] FAIL line=%d: %s\n",__LINE__,#x);return 1;}}while(0)
extern "C" int PsemuKernelSemaphoreSelfTest() {
    const char* old=std::getenv("PSEMU_NATIVE_SEMA");
    struct Restore {std::string value;~Restore(){_putenv_s("PSEMU_NATIVE_SEMA",value.c_str());}} restore{old?old:""};
    _putenv_s("PSEMU_NATIVE_SEMA","1");
    using namespace Libs::LibKernel;
    using namespace Semaphore;
    const auto create=reinterpret_cast<decltype(&KernelCreateSema)>(PsemuKytyGetFunc("188x57JYp0g"));
    const auto wait=reinterpret_cast<decltype(&KernelWaitSema)>(PsemuKytyGetFunc("Zxa0VhQVTsk"));
    const auto signal=reinterpret_cast<decltype(&KernelSignalSema)>(PsemuKytyGetFunc("4czppHBiriw"));
    CHECK(create && wait==&KernelWaitSema && signal==&KernelSignalSema);
    CHECK(PsemuKytyGetFunc("R1Jvn8bSCW8")==reinterpret_cast<void*>(&KernelDeleteSema));
    CHECK(PsemuKytyGetFunc("12wOHk8ywb0")==reinterpret_cast<void*>(&KernelPollSema));
    CHECK(PsemuKytyGetFunc("4DM06U2BNEY")==reinterpret_cast<void*>(&KernelCancelSema));
    KernelSema sem=nullptr;CHECK(create(&sem,nullptr,1,1,8,nullptr)==0 && sem);
    CHECK(KernelPollSema(sem,2)==KERNEL_ERROR_EBUSY);
    CHECK(KernelPollSema(sem,1)==0 && KernelPollSema(sem,1)==KERNEL_ERROR_EBUSY);
    std::atomic<bool> entered{false},done{false};std::atomic<int> result{-1};
    std::thread consumer([&]{entered=true;result=wait(sem,2,nullptr);done=true;});
    while(!entered.load())std::this_thread::yield();Sleep(5);
    const bool before=done.load();CHECK(signal(sem,1)==0);Sleep(5);const bool partial=done.load();
    const int released=signal(sem,1);consumer.join();CHECK(!before && !partial && released==0 && result==0);
    KernelUseconds timeout=1000;CHECK(wait(sem,1,&timeout)==KERNEL_ERROR_ETIMEDOUT && timeout==0);
    CHECK(signal(sem,9)==KERNEL_ERROR_EINVAL && KernelPollSema(sem,1)==KERNEL_ERROR_EBUSY);
    CHECK(wait(sem,0,nullptr)==KERNEL_ERROR_EINVAL && wait(nullptr,1,nullptr)==KERNEL_ERROR_ESRCH);
    entered=false;done=false;std::thread canceled([&]{entered=true;result=wait(sem,8,nullptr);done=true;});
    while(!entered.load())std::this_thread::yield();
    const auto deadline=GetTickCount64()+1000;int waiting=0;
    do {KernelCancelSema(sem,3,&waiting);if(waiting)break;Sleep(1);}while(GetTickCount64()<deadline);
    if(!waiting){KernelDeleteSema(sem);canceled.join();std::puts("[SEMAPHORE-TEST] waiter did not register");return 1;}
    canceled.join();CHECK(result==KERNEL_ERROR_ECANCELED && waiting==1 && KernelPollSema(sem,3)==0);
    CHECK(KernelDeleteSema(sem)==0);
    KernelSema invalid=nullptr;CHECK(create(&invalid,"invalid",1,9,8,nullptr)==KERNEL_ERROR_EINVAL && !invalid);
    std::puts("[SEMAPHORE-TEST] real NID/SysV dispatch, unnamed object, atomic multi-count, real blocking, partial signals, timeout, cancellation, overflow and cleanup passed; game_frame=0");
    return 0;
}
