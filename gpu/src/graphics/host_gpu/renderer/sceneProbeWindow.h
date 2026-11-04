#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
namespace Libs::Graphics {
// Compute uses queue-local submit 0; graphics has globally increasing submit IDs.
// Include Compute between the selected graphics submissions, never anchor at zero.
struct SceneProbeSubmitWindow {
 std::atomic_uint64_t first_graphics{UINT64_MAX};
 std::atomic_bool finished{false};
 bool Include(uint64_t submit, uint64_t submissions = 2) {
  if(submissions==0 || submissions>64)return false;
  if(finished.load())return false;
  if(submit==0)return true;
  auto expected=UINT64_MAX;first_graphics.compare_exchange_strong(expected,submit);
  const auto first=first_graphics.load();
  if(submit>=first && submit-first<submissions)return true;
  if(submit>first)finished.store(true);
  return false;
 }
};
// At most the first 256 bytes of an actually bound constant-buffer view.
inline uint64_t ScenePresentConstantProbeBytes(uint64_t offset,uint64_t range,uint64_t allocation) {
 if((offset&3u)!=0 || offset>allocation)return 0;
 return std::min({range,allocation-offset,uint64_t{256}})&~uint64_t{3};
}
} // namespace Libs::Graphics
