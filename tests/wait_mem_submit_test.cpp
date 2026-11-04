// Production WAIT_MEM body: pending GPU completion must be submitted before polling.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %s\n",#x);std::abort();}}while(0)
#define EXIT(...) std::abort()
#define EXIT_IF(x) CHECK(!(x))
#define LOGF(...) ((void)0)
static unsigned flushes=0,yields=0;
static bool starved=false,require_submit=true;
static std::function<void()> completion;
static unsigned completion_delay=0;
struct CommandProcessor {
 void BufferFlush(){++flushes;if(completion_delay==0)completion();}
 template<class T>void WaitRegMem(uint32_t,const T*,T,T,uint32_t,uint32_t);
 template<class T>void WaitRegMemOld(uint32_t,const T*,T,T,uint32_t,uint32_t);
};
static void YieldCommandProcessorWait(uint32_t){
 ++yields;
 if(yields>32){starved=true;completion();}
 else if(yields>=completion_delay && (!require_submit||flushes!=0))completion();
}
#include "wait_mem_submit.inc"
static unsigned cases=0;
static void Reset(){flushes=yields=0;starved=false;require_submit=true;completion_delay=0;}
template<class T>static void Deferred(bool old,unsigned delay,T reference,T mask){
 Reset();T memory=0;completion_delay=delay;completion=[&]{memory=reference;};CommandProcessor cp;
 if(old)cp.WaitRegMemOld(3,&memory,reference,mask,1,0);
 else cp.WaitRegMem(3,&memory,reference,mask,1,0);
 CHECK(memory==reference);
 if(old)CHECK(starved && flushes==0);
 else CHECK(!starved && flushes==1 && yields==(delay?delay:0));
 ++cases;
}
template<class T>static void AlreadyPassed(uint32_t function,T value,T reference,T mask){
 Reset();completion=[] {CHECK(false);};CommandProcessor cp;
 cp.WaitRegMem(function,&value,reference,mask,17,1);CHECK(flushes==0&&yields==0&&!starved);++cases;
}
int main(){
 Deferred<uint32_t>(true,0,1,UINT32_MAX); // Exact pre-fix negative control starves its label.
 Deferred<uint64_t>(true,0,0x100000005ull,UINT64_MAX);
 Deferred<uint32_t>(false,0,1,UINT32_MAX);
 Deferred<uint64_t>(false,0,0x100000005ull,UINT64_MAX);
 Deferred<uint32_t>(false,3,7,UINT32_MAX);
 Deferred<uint64_t>(false,5,0x8000000000000001ull,UINT64_MAX);
 for(uint32_t f=0;f<=6;++f){
  uint32_t v=f==1||f==2?31:f==4||f==5||f==6?33:32;
  AlreadyPassed<uint32_t>(f,v,32,UINT32_MAX);
  AlreadyPassed<uint64_t>(f,v,32,UINT64_MAX);
 }
 AlreadyPassed<uint64_t>(3,0x1234567800000001ull,1,0xffffffffull);
 AlreadyPassed<uint32_t>(3,0xf0000001u,1,0x0fffffffu);
 std::printf("PASS %u production WAIT_MEM scenarios: 32/64-bit deferred completion, negative controls, one flush, comparisons and masks\n",cases);
}
