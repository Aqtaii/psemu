#include "guest_rwlock.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <vector>
using namespace Psemu::Rwlocks;
static std::array<uint64_t,16> slots{};
static bool Readable(const void* p,size_t n){auto a=reinterpret_cast<uintptr_t>(p),b=reinterpret_cast<uintptr_t>(slots.data());return a>=b && a-b<=sizeof(slots) && n<=sizeof(slots)-(a-b);}
static bool Writable(void* p,size_t n){return Readable(p,n) && p!=&slots[15];}
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
int main(){try{
 Registry api({Readable,Writable});auto call=[&](Operation op,size_t i,uint64_t arg=0){return api.Call(op,reinterpret_cast<uint64_t>(&slots[i]),arg);};
 CHECK(api.Call(Operation::Init,0)==Invalid);CHECK(api.Call(Operation::Read,1)==Invalid);
 CHECK(call(Operation::Init,15)==Invalid);CHECK(slots[15]==0);
 slots[0]=0xfeed;slots[1]=0xdead;slots[2]=0xcafebabe;
 CHECK(call(Operation::AttrInit,0)==0);CHECK(slots[0]!=0xfeed);
 CHECK(call(Operation::Init,1,reinterpret_cast<uint64_t>(&slots[0]))==0);CHECK(slots[2]==0xcafebabe);
 CHECK(call(Operation::Read,2)==Invalid && slots[2]==0xcafebabe);CHECK(call(Operation::Destroy,2)==Invalid);
 CHECK(call(Operation::Init,1)==Busy);CHECK(call(Operation::AttrDestroy,0)==0 && slots[0]==0);
 CHECK(call(Operation::Init,3,reinterpret_cast<uint64_t>(&slots[0]))==Invalid && slots[3]==0);
 CHECK(call(Operation::Read,1)==0);CHECK(call(Operation::Read,1)==0);CHECK(call(Operation::Write,1)==Deadlock);
 CHECK(call(Operation::TryWrite,1)==Busy);CHECK(call(Operation::Destroy,1)==Busy);
 auto shared=std::async(std::launch::async,[&]{CHECK(call(Operation::TryRead,1)==0);CHECK(call(Operation::Unlock,1)==0);return true;});
 CHECK(shared.wait_for(std::chrono::seconds(2))==std::future_status::ready && shared.get());
 std::promise<void> started,acquired;auto ready=started.get_future(),done=acquired.get_future();
 std::thread writer([&]{started.set_value();CHECK(call(Operation::Write,1)==0);acquired.set_value();CHECK(call(Operation::Unlock,1)==0);});
 ready.wait();CHECK(done.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
 CHECK(call(Operation::Unlock,1)==0);CHECK(done.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 CHECK(call(Operation::Unlock,1)==0);CHECK(done.wait_for(std::chrono::seconds(2))==std::future_status::ready);writer.join();
 CHECK(call(Operation::Write,1)==0);
 auto foreign=std::async(std::launch::async,[&]{CHECK(call(Operation::Unlock,1)==Permission);CHECK(call(Operation::TryRead,1)==Busy);CHECK(call(Operation::TryWrite,1)==Busy);return call(Operation::TimedRead,1,1000);});
 CHECK(foreign.wait_for(std::chrono::seconds(2))==std::future_status::ready && foreign.get()==Timeout);
 CHECK(call(Operation::Read,1)==0);CHECK(call(Operation::Unlock,1)==0);CHECK(call(Operation::TryWrite,1)==Busy);CHECK(call(Operation::Unlock,1)==0);
 CHECK(call(Operation::Destroy,1)==0 && slots[1]==0);CHECK(call(Operation::Destroy,1)==Invalid);
 CHECK(call(Operation::Read,4)==0 && slots[4]!=0);CHECK(call(Operation::Unlock,4)==0);CHECK(call(Operation::Destroy,4)==0);
 CHECK(call(Operation::Init,5)==0);int left=0,right=0;std::atomic_bool mismatch=false;
 std::vector<std::thread> workers;
 for(int t=0;t<4;++t)workers.emplace_back([&]{for(int i=0;i<4000;++i){CHECK(call(Operation::Write,5)==0);++left;right=left;CHECK(call(Operation::Unlock,5)==0);}});
 for(int t=0;t<4;++t)workers.emplace_back([&]{for(int i=0;i<4000;++i){CHECK(call(Operation::Read,5)==0);if(left!=right)mismatch=true;CHECK(call(Operation::Unlock,5)==0);}});
 for(auto& t:workers)t.join();CHECK(!mismatch && left==16000 && right==16000);CHECK(call(Operation::Destroy,5)==0);
 CHECK(EncodeResult(Invalid,true)==0x80020016u);CHECK(EncodeResult(Timeout,true)==0x8002003cu);CHECK(EncodeResult(Permission,false)==1);
 std::puts("[RWLOCK-REGRESSION] concurrent_readers=passed exclusive_writers=16000 recursive_read=passed timed_wait=passed owner_destroy_attrs_guards=passed");
}catch(const std::exception& e){std::fprintf(stderr,"RWLOCK FAIL: %s\n",e.what());return 1;}}
