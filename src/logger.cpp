#include "logger.h"
#include <iomanip>
#include <chrono>

std::mutex Logger::log_mutex;

// Yukleme suresi analizi icin: her satirin basina programin basindan bu yana
// gecen saniye. Boylece hangi asamanin ne kadar surdugu logdan dogrudan
// okunabiliyor (tahmin yok).
static double ElapsedSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

static std::string TimeTag() {
    char buf[24];
    snprintf(buf, sizeof(buf), "[T+%7.2f] ", ElapsedSeconds());
    return std::string(buf);
}

void Logger::Init() {
    // Gelecekte dosyaya loglama yazilabilir.
}

void Logger::Info(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << TimeTag() << "[INFO] " << message << std::endl;
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
