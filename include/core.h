#pragma once
#include <cstdint>
#include <windows.h>
#include <map>
#include <string>

// Agresif PLT hooking ile yakalanan oyun thread fonksiyon pointer'i
extern uint64_t g_game_thread_entry;

// PLT indeksinden sembol ismine donusum haritasi
extern std::map<int, std::string> g_plt_names;

// Oyun bellek bolgesinin sinirlarini dogrulamak icin
extern uint64_t g_base_addr;
extern uint64_t g_text_size;

// Oyun icin tahsis edilen TUM bellek blogunun boyutu (max_vaddr_end).
// g_text_size sadece ilk PT_LOAD segmentini kapsar; RET adresi gibi
// "bu adres gercekten oyun modulune mi ait?" kontrolleri icin tum blok gerekir.
extern uint64_t g_module_size;

// PLT#8 (sceKernelGetProcessParam) cagrisinda yakalanan parametreler
// Bunlar orijinal e_entry'nin RSI ve R8 register'larinda bekledigii degerlerdir
extern uint64_t g_plt8_param_ptr;   // R8 -> RSI olarak e_entry'e verilecek
extern uint64_t g_plt8_param_size;  // R9 -> R8 olarak e_entry'e verilecek

// Orijinal ELF e_entry adresi (base_addr + header->e_entry)
extern uint64_t g_original_entry;

// Oyunun kok dizini (eboot.bin'in bulundugu klasor). Guest'teki "/app0/..."
// yollari bu dizine eslenir (VFS).
extern std::string g_game_root;

// DirectMemory havuzunun CPU tabani. GPU descriptor'lari FIZIKSEL adres
// (havuz ici offset) kullanir; CPU adresi = g_dmem_base_addr + phys.
extern uint64_t g_dmem_base_addr;

// Gercek Thread-Local Storage (TLS) blogunun thread pointer (tp) adresi.
// Variant II kuraliyla *(tp) = tp saglanir; negatif offsetler (tp - X) .tdata/.tbss'e erisir.
extern uint64_t g_tls_base;

class Core {
public:
    // VEH'i kaydeder ve Windows Thread'ini baslatarak oyunu yurutur
    static void StartExecution(uint64_t entry_point, uint64_t base_addr, uint64_t text_size, uint64_t original_entry, uint64_t procparam_vaddr,
                                uint64_t tls_vaddr, uint64_t tls_filesz, uint64_t tls_memsz, uint64_t tls_align, uint64_t module_size,
                                uint64_t init_vaddr);

private:
    // Windows tarafindan tetiklenecek Exception (INT 3) isleyicisi
    static LONG WINAPI SyscallExceptionFilter(EXCEPTION_POINTERS* ExceptionInfo);
    
    // Oyun thread'inin ana fonksiyonu (module_start calistirir, sonra e_entry'e sicar)
    static DWORD WINAPI ExecutionThread(LPVOID lpParam);
};
