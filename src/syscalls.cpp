#include "syscalls.h"
#include "logger.h"

uint64_t SyscallManager::HandleSyscall(uint64_t id, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    uint64_t return_value = 0; // Varsayilan olarak basarili dönüyoruz

    switch (id) {
        case 1: // sys_exit
            LOG_INFO("Oyun cikis yapmak istedi (sys_exit).");
            // Emulator dongusu burada bitirilebilir
            break;

        case 5: // sys_open
            Logger::Syscall(id, "sys_open", arg1, arg2, arg3, arg4, arg5, arg6);
            return_value = -1; // simdilik dosya bulamadi gibi dönüyoruz
            break;

        case 11:  // Orbis sys_munmap
        case 477: // Orbis sys_mmap
            Logger::Syscall(id, "sys_mmap / sys_munmap", arg1, arg2, arg3, arg4, arg5, arg6);
            return_value = 0; // Basarili stub
            break;

        case 74: // sys_mprotect
            Logger::Syscall(id, "sys_mprotect", arg1, arg2, arg3, arg4, arg5, arg6);
            return_value = 0;
            break;

        default:
            // Bilinmeyen veya henuz desteklenmeyen bir syscall geldiginde
            Logger::Syscall(id, "UNKNOWN_SYSCALL", arg1, arg2, arg3, arg4, arg5, arg6);
            
            // Oyunun cokmemesi icin stub (kukla) olarak 0 donduruyoruz.
            return_value = 0;
            break;
    }

    return return_value;
}
