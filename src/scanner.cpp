#include "scanner.h"
#include "logger.h"
#include <sstream>

uint64_t Scanner::FindPattern(uint8_t* base_addr, size_t size, const std::string& pattern) {
    std::vector<int> pattern_bytes;
    std::stringstream ss(pattern);
    std::string byte_str;
    
    while (ss >> byte_str) {
        if (byte_str == "?" || byte_str == "??") {
            pattern_bytes.push_back(-1); // Wildcard
        } else {
            pattern_bytes.push_back(std::stoi(byte_str, nullptr, 16));
        }
    }

    if (pattern_bytes.empty()) return 0;

    for (size_t i = 0; i < size - pattern_bytes.size(); ++i) {
        bool found = true;
        for (size_t j = 0; j < pattern_bytes.size(); ++j) {
            if (pattern_bytes[j] != -1 && base_addr[i + j] != static_cast<uint8_t>(pattern_bytes[j])) {
                found = false;
                break;
            }
        }
        // Eger desenin tum baytlari eslestiyse (wildcardlar haric)
        if (found) {
            return reinterpret_cast<uint64_t>(base_addr + i);
        }
    }
    
    return 0; // Bulunamadi
}

uint64_t Scanner::FindEntryPoint(uint8_t* base_addr, size_t text_size) {
    // Standart bir x86_64 fonksiyon baslangici (Prologue):
    // push rbp       (55)
    // mov rbp, rsp   (48 89 E5)
    
    uint64_t entry = FindPattern(base_addr, text_size, "55 48 89 E5");
    if (entry != 0) {
        std::stringstream ss;
        ss << "[SCANNER] Heuristic Entry Point bulundu! Adres: 0x" << std::hex << entry;
        LOG_INFO(ss.str());
        return entry;
    }
    
    return 0;
}
