// Native Fiber regression: no game loading, graphics device or forced guest output.
#include "common/abi.h"
#include "common/commonSubsystem.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "loader/timer.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
extern "C" void* PsemuKytyGetFunc(const char*);
namespace {
using Entry=void (KYTY_SYSV_ABI *)(uint64_t,uint64_t);
using Initialize=int32_t (KYTY_SYSV_ABI *)(void*,const char*,Entry,uint64_t,void*,uint64_t,const void*,uint32_t);
using Run=int32_t (KYTY_SYSV_ABI *)(void*,uint64_t,uint64_t*);
using Return=int32_t (KYTY_SYSV_ABI *)(uint64_t,uint64_t*);
using GetSelf=int32_t (KYTY_SYSV_ABI *)(void**);
Return return_to_thread=nullptr;
GetSelf get_self=nullptr;
void* expected_fiber=nullptr;
uint32_t reached=0;
bool completed=false;
void Require(bool ok){if(!ok){std::fputs("[FIBER-SELF-TEST] failed
",stderr);std::abort();}}
void KYTY_SYSV_ABI TestEntry(uint64_t initialized,uint64_t run){
 volatile uint64_t sentinel=0x1122334455667788ULL;
 uint64_t resumed=run;
 for(uint32_t i=0;i<64;++i){
  void* self=nullptr;
  Require(initialized==100 && resumed==i+1 && sentinel==0x1122334455667788ULL);
  Require(get_self(&self)==0 && self==expected_fiber);
  reached=i+1;
  Require(return_to_thread(initialized+resumed+i,&resumed)==0);
 }
 completed=true;
}
}
extern "C" int PsemuFiberSelfTest(){
 _putenv_s("PSEMU_NATIVE_FIBER","1");
 auto* list=Common::SubsystemsList::Instance();
 auto* core=Common::CommonSubsystem::Instance();
 auto* threads=Common::ThreadsSubsystem::Instance();
 auto* config=Config::ConfigSubsystem::Instance();
 auto* log=Log::LogSubsystem::Instance();
 auto* timer=Loader::Timer::TimerSubsystem::Instance();
 list->Add(core,{});list->Add(threads,{core});list->InitAll(true);
 list->Add(config,{core});list->InitAll(true);
 list->Add(log,{core,config});list->Add(timer,{core,log});list->InitAll(true);
 auto initialize=reinterpret_cast<Initialize>(PsemuKytyGetFunc("hVYD7Ou2pCQ"));
 auto run=reinterpret_cast<Run>(PsemuKytyGetFunc("a0LLrZWac0M"));
 return_to_thread=reinterpret_cast<Return>(PsemuKytyGetFunc("B0ZX2hx9DMw"));
 get_self=reinterpret_cast<GetSelf>(PsemuKytyGetFunc("p+zLIOg27zU"));
 Require(initialize && run && return_to_thread && get_self);
 alignas(16) unsigned char fiber[256]{};
 expected_fiber=fiber;
 void* stack=VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 Require(stack!=nullptr);
 Require(initialize(nullptr,"invalid",TestEntry,100,stack,65536,nullptr,0)!=0);
 Require(initialize(fiber,"psemu regression",TestEntry,100,stack,65536,nullptr,0)==0);
 for(uint32_t i=0;i<64;++i){
  uint64_t returned=0;
  Require(run(fiber,i+1,&returned)==0);
  Require(returned==100+(i+1)+i && reached==i+1 && !completed);
  void* self=reinterpret_cast<void*>(1);
  Require(get_self(&self)==0 && self==nullptr);
 }
 uint64_t returned=99;
 Require(run(fiber,65,&returned)!=0 && completed && returned==0);
 VirtualFree(stack,0,MEM_RELEASE);
 std::puts("[FIBER-SELF-TEST] native_nid=1 stack_preserved=1 yields=64 resumes=64 termination=1 game_loaded=0");
 return 0;
}
