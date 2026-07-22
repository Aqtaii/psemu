#pragma once
#include <cstdint>

class SyscallManager {
public:
    // PS5 (FreeBSD bazli) Syscall isleyicisi.
    // Sonuclari RAX uzerinden doner.
    // System V AMD64 ABI: Argumanlar sirasiyla RDI, RSI, RDX, R10, R8, R9 uzerinden gelir.
    // (Not: Kullanici alaninda 4. arguman RCX'tir ancak Syscall tetiklenirken FreeBSD kernel'i R10 kullanir)
    static uint64_t HandleSyscall(uint64_t id, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
};
