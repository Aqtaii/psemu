#pragma once
#include <atomic>

namespace Libs::Graphics {
// Claiming a request changes the atomic before the caller publishes its payload.
// wait() may observe that intermediate value; only the later release-store of
// Requested/InitRequested makes the payload available. Callers retain validation
// of every non-waiting state and the existing fault-transaction serialization.
template<class AtomicState, class... PendingStates>
auto WaitForReadbackWork(AtomicState& state, PendingStates... pending) noexcept {
 auto current=state.load(std::memory_order_acquire);
 while(((current==pending) || ...)) {
  state.wait(current,std::memory_order_acquire);
  current=state.load(std::memory_order_acquire);
 }
 return current;
}
} // namespace Libs::Graphics
