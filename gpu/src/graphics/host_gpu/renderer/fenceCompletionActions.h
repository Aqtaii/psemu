#pragma once
#include <functional>
#include <utility>
#include <vector>
namespace Libs::Graphics {
// Owner calls Complete only after submitted GPU work's fence. Discarded
// recordings release captures without observing unexecuted GPU results.
class FenceCompletionActions {
public:
 void Add(std::function<void()> action) { m_actions.push_back(std::move(action)); }
 void Complete() {
  std::vector<std::function<void()>> completed;
  completed.swap(m_actions);
  for(auto& action: completed) action();
 }
 bool Empty() const noexcept {return m_actions.empty();}
 void Discard() noexcept { m_actions.clear(); }
private:
 std::vector<std::function<void()>> m_actions;
};
}
