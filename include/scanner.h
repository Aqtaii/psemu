#pragma once
#include <vector>
#include <string>
#include <cstdint>

class Scanner {
public:
    // Wildcard (?) destekli bellek imza tarayicisi
    // Ornek Pattern: "55 48 89 E5 ? ? ? 48 83 EC"
    static uint64_t FindPattern(uint8_t* base_addr, size_t size, const std::string& pattern);
    
    // Oyunun gercek module_start adresini heuristic (sezgisel) olarak bulur
    static uint64_t FindEntryPoint(uint8_t* base_addr, size_t text_size);
};
