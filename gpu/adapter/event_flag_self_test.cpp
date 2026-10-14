#include "kernel/eventFlag.h"
#include "libs/errno.h"
#include <thread>
#include <atomic>
#include <cstdio>
extern "C" int PsemuEventFlagSelfTest() {
    using namespace Libs::LibKernel::EventFlag;
    KernelEventFlag flag = nullptr;
    if (KernelCreateEventFlag(&flag,"psemu-event-test",0x21,0,nullptr)!=0 || !flag) return 1;
    uint64_t pattern = 0;
    if (KernelPollEventFlag(flag,1,2,&pattern)!=Libs::LibKernel::KERNEL_ERROR_EBUSY || pattern!=0) return 2;
    std::atomic<int> waiting{-1};
    std::atomic<bool> entered{false};
    std::thread thread([&] {
        uint32_t timeout = 1000000;
        entered.store(true,std::memory_order_release);
        waiting.store(KernelWaitEventFlag(flag,3,1,&pattern,&timeout),std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    KernelSetEventFlag(flag,1);
    KernelSetEventFlag(flag,2);
    thread.join();
    if (waiting.load()!=0 || pattern!=3) return 3;
    if (KernelPollEventFlag(flag,1,0x21,&pattern)!=0 || pattern!=3) return 4;
    if (KernelPollEventFlag(flag,1,1,&pattern)!=Libs::LibKernel::KERNEL_ERROR_EBUSY || pattern!=2) return 5;
    if (KernelClearEventFlag(flag,~uint64_t(2))!=0) return 6;
    uint32_t timeout = 1000;
    if (KernelWaitEventFlag(flag,1,1,&pattern,&timeout)!=Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT || timeout!=0) return 7;
    int canceled = -1;
    if (KernelCancelEventFlag(flag,4,&canceled)!=0 || canceled!=0) return 8;
    if (KernelPollEventFlag(flag,4,1,&pattern)!=0 || pattern!=4) return 9;
    if (KernelDeleteEventFlag(flag)!=0) return 10;
    std::puts("Event flag create, wait, set, poll, clear, timeout, cancel and delete passed");
    return 0;
}
