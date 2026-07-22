#include "logger.h"
#include <iomanip>

std::mutex Logger::log_mutex;

void Logger::Init() {
    // Gelecekte dosyaya loglama yazilabilir.
}

void Logger::Info(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "[INFO] " << message << std::endl;
}

void Logger::Error(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << "[-] ERROR: " << message << std::endl;
}

void Logger::Syscall(int id, const std::string& name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "\n[SYSCALL] ID: " << id << " (" << name << ")\n"
              << "   -> Argümanlar: " << std::hex 
              << "0x" << arg1 << ", 0x" << arg2 << ", 0x" << arg3 
              << ", 0x" << arg4 << ", 0x" << arg5 << ", 0x" << arg6 
              << std::dec << std::endl;
}
