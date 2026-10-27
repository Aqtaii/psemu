#include "kernel/pthread.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
extern "C" void* PsemuKytyGetFunc(const char* nid);
#define CHECK(x) do {if(!(x)){std::printf("[CLOCK-TEST] FAIL line=%d: %s\n",__LINE__,#x);return 1;}}while(0)
extern "C" int PsemuClockSelfTest() {
    const char* old=std::getenv("PSEMU_NATIVE_CLOCK");
    struct Restore {std::string value;~Restore(){_putenv_s("PSEMU_NATIVE_CLOCK",value.c_str());}} restore{old?old:""};
    _putenv_s("PSEMU_NATIVE_CLOCK","1");
    using namespace Libs::LibKernel;
    const auto ticks=reinterpret_cast<decltype(&KernelReadTsc)>(PsemuKytyGetFunc("-2IRUCO--PM"));
    const auto frequency=reinterpret_cast<decltype(&KernelGetTscFrequency)>(PsemuKytyGetFunc("1j3S3n-tTW4"));
    const auto microseconds=reinterpret_cast<decltype(&KernelGetProcessTime)>(PsemuKytyGetFunc("4J2sUJmuHZQ"));
    const auto counter=reinterpret_cast<decltype(&KernelGetProcessTimeCounter)>(PsemuKytyGetFunc("fgxnMeTNUtY"));
    const auto counter_frequency=reinterpret_cast<decltype(&KernelGetProcessTimeCounterFrequency)>(PsemuKytyGetFunc("BNowx2l588E"));
    CHECK(ticks==&KernelReadTsc && frequency==&KernelGetTscFrequency && microseconds==&KernelGetProcessTime &&
          counter==&KernelGetProcessTimeCounter && counter_frequency==&KernelGetProcessTimeCounterFrequency);
    const auto freq=frequency();CHECK(freq && freq==counter_frequency());
    microseconds();counter(); // Initialize the shared native process epoch before measurement.
    LARGE_INTEGER qfreq{},q0{},q1{};CHECK(QueryPerformanceFrequency(&qfreq));
    CHECK(QueryPerformanceCounter(&q0));const auto t0=ticks(),c0=counter(),u0=microseconds();
    Sleep(30);
    const auto t1=ticks(),c1=counter(),u1=microseconds();CHECK(QueryPerformanceCounter(&q1));
    CHECK(t1>t0 && c1>c0 && u1>u0);
    const double wall=double(q1.QuadPart-q0.QuadPart)/double(qfreq.QuadPart);
    CHECK(std::abs(double(t1-t0)/double(freq)-wall)<0.01);
    CHECK(std::abs(double(c1-c0)/double(freq)-double(u1-u0)/1000000.0)<0.01);
    // Same counter epoch and units as VideoOut processTimeCounter/processTime.
    const auto native_status_counter=KernelGetProcessTimeCounter();const auto imported_counter=counter();
    CHECK(imported_counter>=native_status_counter && imported_counter-native_status_counter<freq/10);
    const auto native_status_us=KernelGetProcessTime();const auto imported_us=microseconds();
    CHECK(imported_us>=native_status_us && imported_us-native_status_us<100000);
    CHECK(std::abs(double(imported_counter)/double(freq)-double(imported_us)/1000000.0)<0.1);
    std::printf("[CLOCK-TEST] native_hz=%llu qpc_hz=%llu shared VideoOut epoch, real NID/SysV pointers, monotonic ticks and microseconds passed; game_frame=0\n",
                static_cast<unsigned long long>(freq),static_cast<unsigned long long>(qfreq.QuadPart));
    return 0;
}
