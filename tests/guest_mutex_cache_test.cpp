int main(){
 auto* arena=static_cast<uint8_t*>(VirtualAlloc(nullptr,65536,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));CHECK(arena);
 std::array<uint64_t*,32> slots{};std::array<GuestMutex*,32> mutexes{};
 for(size_t i=0;i<slots.size();++i){slots[i]=reinterpret_cast<uint64_t*>(arena+i*256);mutexes[i]=GetOrCreateMutex(slots[i]);CHECK(mutexes[i]);}
 const auto before=g_vq_calls.load();
 for(unsigned pass=0;pass<1000;++pass)for(size_t i=0;i<slots.size();++i)CHECK(GetOrCreateMutex(slots[i])==mutexes[i]);
 CHECK(g_vq_calls.load()==before);
 const auto original=*slots[0];*slots[0]=*slots[1];CHECK(GetOrCreateMutex(slots[0])==mutexes[1]);*slots[0]=original;CHECK(GetOrCreateMutex(slots[0])==mutexes[0]);
 CHECK(NativeMutexTrylock(slots[0])==0);CHECK(NativeMutexUnlock(slots[0])==0);
 std::atomic<bool> held{false},release{false};std::thread owner([&]{NativeMutexLock(slots[0]);held=true;while(!release.load())std::this_thread::yield();NativeMutexUnlock(slots[0]);});
 while(!held.load())std::this_thread::yield();const int busy=NativeMutexTrylock(slots[0]);release=true;owner.join();CHECK(busy==16);
 CHECK(NativeMutexTrylock(slots[0])==0);NativeMutexUnlock(slots[0]);
 int shared=0;std::array<std::thread,8> workers;
 for(auto& worker:workers)worker=std::thread([&]{for(int i=0;i<1000;++i){NativeMutexLock(slots[0]);++shared;NativeMutexUnlock(slots[0]);}});
 for(auto& worker:workers)worker.join();CHECK(shared==8000);
 DWORD old=0;CHECK(VirtualProtect(arena+32768,4096,PAGE_NOACCESS,&old));CHECK(!GetOrCreateMutex(reinterpret_cast<uint64_t*>(arena+32768)));CHECK(!GetOrCreateMutex(nullptr));
 MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(arena+32768,&info,sizeof(info)));CHECK(info.Protect==PAGE_NOACCESS);
 CHECK(VirtualFree(arena,0,MEM_RELEASE));
 std::puts("PASS: 32 formerly colliding slots, 32000 warm lookups with zero VirtualQuery, slot reuse, actual SysV contention/EBUSY, eight-thread exclusion, inaccessible slot rejection");
}
