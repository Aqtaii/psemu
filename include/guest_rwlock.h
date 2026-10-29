#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Psemu::Rwlocks {
// Error numbers and writer preference follow the pinned KytyPS5 pthread contract.
inline constexpr int Invalid=22,Busy=16,Deadlock=11,Permission=1,Timeout=60,NoMemory=12,Again=35;
enum class Operation { AttrInit,AttrDestroy,Init,Destroy,Read,Write,TryRead,TryWrite,Unlock,TimedRead,TimedWrite };
inline uint32_t EncodeResult(int result,bool sce) {return result==0 ? 0u : (sce ? 0x80020000u|uint32_t(result) : uint32_t(result));}

class State {
 std::mutex mutex;
 std::condition_variable changed;
 std::unordered_map<std::thread::id,uint32_t> readers;
 std::thread::id writer;
 uint64_t reader_count=0;
 uint32_t waiting_readers=0,waiting_writers=0;
 bool closed=false;
public:
 int Lock(bool write,bool attempt=false,bool timed=false,uint32_t usec=0) {
  const auto self=std::this_thread::get_id();
  std::unique_lock lock(mutex);
  if(closed)return Invalid;
  if(write && (writer==self || readers.contains(self)))return attempt ? Busy : Deadlock;
  auto ready=[&]{return write ? writer==std::thread::id{} && reader_count==0
   : writer==self || (writer==std::thread::id{} && (waiting_writers==0 || readers.contains(self)));};
  if(!ready()) {
   if(attempt)return Busy;
   auto& waiting=write ? waiting_writers : waiting_readers;
   ++waiting;
   bool acquired=true;
   if(timed)acquired=changed.wait_for(lock,std::chrono::microseconds(usec),[&]{return closed || ready();});
   else changed.wait(lock,[&]{return closed || ready();});
   --waiting;
   if(!acquired){changed.notify_all();return Timeout;}
   if(closed)return Invalid;
  }
  if(write)writer=self;
  else {
   auto it=readers.find(self);
   if(it!=readers.end() && it->second==(std::numeric_limits<uint32_t>::max)())return Again;
   if(it==readers.end())readers.emplace(self,1);else ++it->second;
   ++reader_count;
  }
  return 0;
 }
 int Unlock() {
  std::lock_guard lock(mutex);
  if(closed)return Invalid;
  const auto self=std::this_thread::get_id();
  if(writer==self)writer={};
  else {
   auto it=readers.find(self);
   if(it==readers.end())return Permission;
   --reader_count;
   if(--it->second==0)readers.erase(it);
  }
  changed.notify_all();return 0;
 }
 int Destroy() {
  std::lock_guard lock(mutex);
  if(closed)return Invalid;
  if(writer!=std::thread::id{} || reader_count || waiting_readers || waiting_writers)return Busy;
  closed=true;return 0;
 }
};

struct MemoryAccess {bool(*readable)(const void*,size_t);bool(*writable)(void*,size_t);};
class Registry {
 struct Attribute {};
 MemoryAccess memory;
 std::mutex mutex;
 std::unordered_map<uint64_t,std::shared_ptr<State>> locks;
 std::unordered_map<uint64_t,std::unique_ptr<Attribute>> attributes;
 bool Slot(uint64_t* slot) const {
  return slot && memory.readable(slot,sizeof(*slot)) && memory.writable(slot,sizeof(*slot));
 }
public:
 explicit Registry(MemoryAccess access):memory(access){}
 int Call(Operation op,uint64_t address,uint64_t argument=0) {
  auto* slot=reinterpret_cast<uint64_t*>(address);
  if(!Slot(slot))return Invalid;
  try {
   std::shared_ptr<State> state;
   {
    std::lock_guard lock(mutex);
    const auto value=*slot;
    if(op==Operation::AttrInit) {
     if(attributes.contains(value))return Busy;
     auto attr=std::make_unique<Attribute>();const auto handle=reinterpret_cast<uint64_t>(attr.get());
     attributes.emplace(handle,std::move(attr));*slot=handle;return 0;
    }
    if(op==Operation::AttrDestroy) {
     if(!attributes.erase(value))return Invalid;
     *slot=0;return 0;
    }
    auto it=locks.find(value);
    if(op==Operation::Init) {
     if(it!=locks.end())return Busy;
     if(argument) {
      auto* attr=reinterpret_cast<const uint64_t*>(argument);
      if(!memory.readable(attr,sizeof(*attr)) || !attributes.contains(*attr))return Invalid;
     }
     state=std::make_shared<State>();const auto handle=reinterpret_cast<uint64_t>(state.get());
     locks.emplace(handle,state);*slot=handle;return 0;
    }
    if(it==locks.end()) {
     const bool acquire=op!=Operation::Destroy && op!=Operation::Unlock;
     if(value!=0 || !acquire)return Invalid;
     state=std::make_shared<State>();const auto handle=reinterpret_cast<uint64_t>(state.get());
     locks.emplace(handle,state);*slot=handle;
    } else state=it->second;
    if(op==Operation::Destroy) {
     const auto result=state->Destroy();
     if(result==0){locks.erase(value);*slot=0;}
     return result;
    }
   }
   switch(op) {
    case Operation::Read:return state->Lock(false);
    case Operation::Write:return state->Lock(true);
    case Operation::TryRead:return state->Lock(false,true);
    case Operation::TryWrite:return state->Lock(true,true);
    case Operation::Unlock:return state->Unlock();
    case Operation::TimedRead:return state->Lock(false,false,true,uint32_t(argument));
    case Operation::TimedWrite:return state->Lock(true,false,true,uint32_t(argument));
    default:return Invalid;
   }
  } catch(const std::bad_alloc&) {return NoMemory;}
 }
};
}
