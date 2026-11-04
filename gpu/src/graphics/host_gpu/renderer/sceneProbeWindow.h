#pragma once
#include <atomic>
#include <cstdint>
namespace Libs::Graphics {
// Compute uses queue-local submit 0; graphics has globally increasing submit IDs.
// Include Compute between the selected graphics submissions, never anchor at zero.
struct SceneProbeSubmitWindow {
 std::atomic_uint64_t first_graphics{UINT64_MAX};
 std::atomic_bool finished{false};
 bool Include(uint64_t submit) {
  if(finished.load())return false;
  if(submit==0)return true;
  auto expected=UINT64_MAX;first_graphics.compare_exchange_strong(expected,submit);
  const auto first=first_graphics.load();
  if(submit>=first && submit-first<2)return true;
  if(submit>first)finished.store(true);
  return false;
 }
};
} // namespace Libs::Graphics
