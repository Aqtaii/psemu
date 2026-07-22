#include "linker.h"
#include "scanner.h"
#include "logger.h"

// Sahte (Stub) kütüphane fonksiyonlarımız
extern "C" uint64_t Stub_sceLibcInit() {
    LOG_INFO("[PRX] libSceLibc -> sceLibcInit() çağrıldı! (Stubbed)");
    return 0;
}

extern "C" uint64_t Stub_libkernel_init() {
    LOG_INFO("[PRX] libkernel -> libkernel_init() çağrıldı! (Stubbed)");
    return 0;
}

#include <windows.h>
#include <vector>

// SEH (__try / __except) bloklari C++ objeleri (std::vector vb.) iceren 
// fonksiyonlarda dogrudan kullanilamaz. Bu yuzden ayri bir helper yaziyoruz.
static bool PatchGOT(uint64_t got_addr, uint64_t magic_addr) {
    __try {
        DWORD oldProt;
        if (VirtualProtect(reinterpret_cast<void*>(got_addr), 8, PAGE_READWRITE, &oldProt)) {
            *reinterpret_cast<uint64_t*>(got_addr) = magic_addr;
            VirtualProtect(reinterpret_cast<void*>(got_addr), 8, oldProt, &oldProt);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

void Linker::ResolveImports(uint8_t* base_addr, size_t text_size) {
    LOG_INFO("[LINKER] Dinamik kutuphane cagrilari (PRX Imports) cozumleniyor...");
    LOG_INFO("[LINKER] PLT/GOT Hook altyapisi kuruluyor...");
    
    // Debug: text_size'i goster
    printf("[DEBUG-LINKER] base_addr=0x%llx, text_size=0x%llx\n", 
           (unsigned long long)base_addr, (unsigned long long)text_size);
    fflush(stdout);
    
    if (text_size < 32) {
        LOG_ERROR("[LINKER] text_size cok kucuk veya sifir! PLT taramasi iptal edildi.");
        return;
    }
    
    int hook_count = 0;
    int skip_count = 0;
    
    static const int pattern[] = {0xFF, 0x25, -1, -1, -1, -1, 0x68, -1, -1, -1, -1, 0xE9};
    const size_t pattern_size = sizeof(pattern) / sizeof(pattern[0]);
    
    printf("[DEBUG-LINKER] Tarama basliyor...\n");
    fflush(stdout);
    
    for (size_t i = 0; i < text_size - 16; ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern_size; ++j) {
            if (pattern[j] != -1 && base_addr[i + j] != static_cast<uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }
        
        if (match) {
            int32_t rel_offset = *reinterpret_cast<int32_t*>(&base_addr[i + 2]);
            uint64_t rip_next = reinterpret_cast<uint64_t>(&base_addr[i + 6]);
            uint64_t got_addr = rip_next + static_cast<int64_t>(rel_offset);
            
            int32_t plt_index = *reinterpret_cast<int32_t*>(&base_addr[i + 7]);
            uint64_t magic_addr = 0x10000000000ULL + static_cast<uint64_t>(plt_index);
            
            // Her 50 match'te bir ilerleme raporu ver
            if (hook_count % 50 == 0) {
                printf("[DEBUG-LINKER] Match #%d @ offset 0x%llx -> GOT=0x%llx, idx=%d\n",
                       hook_count, (unsigned long long)i, (unsigned long long)got_addr, plt_index);
                fflush(stdout);
            }
            
            if (PatchGOT(got_addr, magic_addr)) {
                hook_count++;
            } else {
                skip_count++;
            }
            
            i += 15;
        }
    }
    
    printf("[DEBUG-LINKER] Tarama bitti! hook=%d, skip=%d\n", hook_count, skip_count);
    fflush(stdout);
    
    LOG_INFO("[LINKER] Toplam " + std::to_string(hook_count) + " adet PLT/GOT kancasi (Stub) atildi!");
}
