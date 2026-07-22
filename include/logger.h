#pragma once
#include <iostream>
#include <string>
#include <mutex>
#include <cstdint>

class Logger {
public:
    static void Init();
    static void Info(const std::string& message);
    static void Error(const std::string& message);
    
    // Syscall cagrilarini detayli loglamak icin ozel fonksiyon
    static void Syscall(int id, const std::string& name, 
                        uint64_t arg1, uint64_t arg2, uint64_t arg3, 
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);

private:
    static std::mutex log_mutex;
};

// Kod icinde kullanimi kolaylastiran Makrolar
#define LOG_INFO(msg) Logger::Info(msg)
#define LOG_ERROR(msg) Logger::Error(msg)
