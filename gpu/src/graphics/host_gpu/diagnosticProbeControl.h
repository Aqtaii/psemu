#pragma once
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace Libs::Graphics {
// Optional gate for diagnostics only. Absence of the environment setting retains
// existing behavior; a configured missing file preserves the late-draw budget.
inline bool AstroFrameProbeGateOpen() {
 static const std::filesystem::path gate=[] {
  const auto* path=std::getenv("PSEMU_ASTRO_PROBE_GATE");
  return path && path[0] ? std::filesystem::path(path) : std::filesystem::path{};
 }();
 if (gate.empty()) return true;
 std::error_code error;
 return std::filesystem::is_regular_file(gate,error) && !error;
}
} // namespace Libs::Graphics
