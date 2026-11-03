#include "graphics/host_gpu/renderer/readbackWorkerWait.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include "readback_wait_calls.inc"
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
template<class State> struct ObservedAtomic {
 using value_type=State;
 std::atomic<State> value;
 std::counting_semaphore<1024> entered{0};
 std::atomic<State> last;
 bool observe=true;
 explicit ObservedAtomic(State s):value(s),last(s){}
 State load(std::memory_order order)const{return value.load(order);}
 void wait(State old,std::memory_order order){
  if(observe){last.store(old,std::memory_order_release);entered.release();}
  value.wait(old,order);
 }
 void WaitEntered(State expected){
  if(!entered.try_acquire_for(3s)){std::fputs("FAIL worker did not enter atomic wait\n",stderr);std::abort();}
  CHECK(last.load(std::memory_order_acquire)==expected);
 }
};
using Payload=std::array<uint32_t,66>;
static Payload MakePayload(uint32_t tag){
 Payload p;p.fill(0xa55aa55a);
 for(size_t i=1;i+1<p.size();++i)p[i]=tag+uint32_t(i*37);
 return p;
}
template<class State,class Wait>
static void HeldClaim(const char*name,Wait wait,State claimed,State published,State stopped){
 ObservedAtomic<State> state(claimed);Payload payload=MakePayload(0),observed{};
 std::atomic_bool done=false;State returned=stopped;
 std::thread worker([&]{returned=wait(state);observed=payload;done.store(true,std::memory_order_release);});
 state.WaitEntered(claimed);CHECK(!done.load(std::memory_order_acquire));
 // A notification is not payload publication.
 state.value.notify_all();CHECK(!done.load(std::memory_order_acquire));
 payload=MakePayload(0x12340000);
 state.value.store(published,std::memory_order_release);state.value.notify_all();
 worker.join();CHECK(returned==published && observed==payload && done.load());
 std::printf("[READBACK-WAIT] %s held_claim=wait published_payload=exact\n",name);
}
template<class State,class Wait>
static void Transition(const char*name,Wait wait,State initial,State claimed,State published,State stopped){
 ObservedAtomic<State> state(initial);Payload payload=MakePayload(0),observed{};
 std::atomic_bool done=false;State returned=stopped;
 std::thread worker([&]{returned=wait(state);observed=payload;done.store(true,std::memory_order_release);});
 state.WaitEntered(initial);
 state.value.store(claimed,std::memory_order_release);state.value.notify_all();
 state.WaitEntered(claimed);CHECK(!done.load(std::memory_order_acquire));
 payload=MakePayload(0x56780000);state.value.store(published,std::memory_order_release);state.value.notify_all();
 worker.join();CHECK(returned==published && observed==payload);
 std::printf("[READBACK-WAIT] %s early_wake=claim_wait published_payload=exact\n",name);
}
template<class State,class Wait>
static void Stop(const char*name,Wait wait,State initial,State stopping,State stopped){
 ObservedAtomic<State> state(initial);State returned=stopped;
 std::thread worker([&]{returned=wait(state);});
 state.WaitEntered(initial);state.value.store(stopping,std::memory_order_release);state.value.notify_all();
 worker.join();CHECK(returned==stopping);
 std::printf("[READBACK-WAIT] %s stopping_observed=1\n",name);
}
template<class State,class Wait>
static void Stress(const char*name,Wait wait){
 constexpr uint32_t count=10000;
 ObservedAtomic<State> state(State::Idle);state.observe=false;
 Payload payload{},observed{};std::atomic_bool valid=true,finished=false;
 std::thread watchdog([&]{
  const auto deadline=std::chrono::steady_clock::now()+8s;
  while(!finished.load(std::memory_order_acquire)){
   if(std::chrono::steady_clock::now()>deadline){std::fputs("FAIL readback wait stress timeout\n",stderr);std::abort();}
   std::this_thread::sleep_for(10ms);
  }
 });
 std::thread worker([&]{
  for(uint32_t n=1;n<=count;++n){
   const auto current=wait(state);
   if(current!=State::Requested)valid.store(false);
   observed=payload;if(observed!=MakePayload(n*101))valid.store(false);
   state.value.store(State::Idle,std::memory_order_release);state.value.notify_all();
  }
 });
 for(uint32_t n=1;n<=count;++n){
  auto expected=State::Idle;
  while(!state.value.compare_exchange_weak(expected,State::Claimed,std::memory_order_acq_rel)){
   if(expected!=State::Idle)state.value.wait(expected,std::memory_order_acquire);
   expected=State::Idle;
  }
  payload=MakePayload(n*101);
  state.value.store(State::Requested,std::memory_order_release);state.value.notify_all();
 }
 worker.join();finished.store(true,std::memory_order_release);watchdog.join();
 CHECK(valid.load() && state.load(std::memory_order_acquire)==State::Idle);
 std::printf("[READBACK-WAIT] %s requests=%u all_payloads_and_guards_exact=1\n",name,count);
}
int main(){
 try{
  auto buffer=[](auto& s){return WaitBufferProduction(s);};
  auto texture=[](auto& s){return WaitTextureProduction(s);};
  {
   ObservedAtomic<BufferState> s(BufferState::Claimed);
   CHECK(WaitBufferOld(s)==BufferState::Claimed);
   CHECK(!s.entered.try_acquire());
   std::printf("[READBACK-WAIT] old_buffer_control returned_unsupported_claim=%u\n",unsigned(BufferState::Claimed));
  }
  {
   ObservedAtomic<TextureState> s(TextureState::Claimed);
   CHECK(WaitTextureOld(s)==TextureState::Claimed);CHECK(!s.entered.try_acquire());
   std::printf("[READBACK-WAIT] old_texture_control returned_unsupported_claim=%u\n",unsigned(TextureState::Claimed));
  }
  HeldClaim("buffer-request",buffer,BufferState::Claimed,BufferState::Requested,BufferState::Stopped);
  HeldClaim("buffer-init",buffer,BufferState::Claimed,BufferState::InitRequested,BufferState::Stopped);
  HeldClaim("texture-request",texture,TextureState::Claimed,TextureState::Requested,TextureState::Stopped);
  Transition("buffer-idle-claim",buffer,BufferState::Idle,BufferState::Claimed,BufferState::Requested,BufferState::Stopped);
  Transition("buffer-uninitialized-claim",buffer,BufferState::Uninitialized,BufferState::Claimed,BufferState::InitRequested,BufferState::Stopped);
  Transition("texture-idle-claim",texture,TextureState::Idle,TextureState::Claimed,TextureState::Requested,TextureState::Stopped);
  Stop("buffer-stop-idle",buffer,BufferState::Idle,BufferState::Stopping,BufferState::Stopped);
  Stop("buffer-stop-uninitialized",buffer,BufferState::Uninitialized,BufferState::Stopping,BufferState::Stopped);
  Stop("texture-stop",texture,TextureState::Idle,TextureState::Stopping,TextureState::Stopped);
  // Unexpected protocol states must still reach the caller's validation.
  for(auto x:{BufferState::Ready,BufferState::Installed,BufferState::Completed,BufferState::Stopped}){
   ObservedAtomic<BufferState>s(x);CHECK(buffer(s)==x);CHECK(!s.entered.try_acquire());
  }
  for(auto x:{TextureState::Ready,TextureState::Installed,TextureState::Completed,TextureState::Stopped}){
   ObservedAtomic<TextureState>s(x);CHECK(texture(s)==x);CHECK(!s.entered.try_acquire());
  }
  Stress<BufferState>("buffer-stress",buffer);Stress<TextureState>("texture-stress",texture);
  std::puts("PASS actual production wait calls: old Claimed controls, held publication, early wake, initialization, stopping, unexpected-state validation, 20000 payload handoffs");
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
