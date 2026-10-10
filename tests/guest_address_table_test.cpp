#include "graphics/host_gpu/renderer/guestAddressTable.h"
#include "graphics/host_gpu/renderer/fenceCompletionActions.h"
#include "graphics/host_gpu/renderer/guestAddressFaultRecord.h"
#include <memory>
#include <cstdio>
#include <array>
using namespace Libs::Graphics;
#define CHECK(x) do {if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
 FenceCompletionActions actions;
 int completed=0;
 auto resource=std::make_shared<int>(42);std::weak_ptr<int> weak=resource;
 actions.Add([resource,&completed]{if(*resource==42)++completed;});
 resource.reset();CHECK(!weak.expired());CHECK(completed==0);
 actions.Complete();CHECK(completed==1);CHECK(weak.expired());
 actions.Complete();CHECK(completed==1);
 resource=std::make_shared<int>(7);weak=resource;
 actions.Add([resource,&completed]{++completed;});resource.reset();
 actions.Discard();CHECK(weak.expired());actions.Complete();CHECK(completed==1);
 CHECK(CanPrepareGuestAddressFault({1,0x56789abc,0x1234,0}));
 CHECK(!CanPrepareGuestAddressFault({0,0x56789abc,0x1234,0}));
 CHECK(!CanPrepareGuestAddressFault({2,0x56789abc,0x1234,0}));
 CHECK(!CanPrepareGuestAddressFault({1,0,0,0}));
 CHECK(!CanPrepareGuestAddressFault({1,4,0x10000,0}));
 CHECK(!CanPrepareGuestAddressFault({1,5,0,0}));
 CHECK(!CanPrepareGuestAddressFault({1,4,0,1}));
 const uint64_t base=0x265a0000000ull;
 std::vector<GuestAddressRange> table;
 const std::array input{GuestAddressRange{base+16384,0x90004000,16384,0},
                        GuestAddressRange{base,0x80000000,16384,0}};
 CHECK(BuildGuestAddressTable(input,&table));CHECK(table[0].guest_base==base);
 uint64_t out=123;
 CHECK(ResolveGuestAddress(table,base,4,&out));CHECK(out==0x80000000);
 CHECK(ResolveGuestAddress(table,base+16380,4,&out));CHECK(out==0x80003ffc);
 CHECK(!ResolveGuestAddress(table,base+16380,8,&out));CHECK(out==0x80003ffc);
 // Two blocks in one guest BVH node can live in discontiguous device buffers.
 CHECK(ResolveGuestAddress(table,base+16320,64,&out));CHECK(out==0x80003fc0);
 CHECK(ResolveGuestAddress(table,base+16384,64,&out));CHECK(out==0x90004000);
 CHECK(!ResolveGuestAddress(table,base+32768,4,&out));
 CHECK(!ResolveGuestAddress(table,base-1,4,&out));
 CHECK(!ResolveGuestAddress(table,base,0,&out));
 CHECK(!ResolveGuestAddress(table,base,4,nullptr));
 const auto saved=table;
 for(auto bad: std::array{GuestAddressRange{0,1,1,0},GuestAddressRange{base,0,4,0},
   GuestAddressRange{base,1,0,0},GuestAddressRange{GuestAddressLimit-1,1,2,0},
   GuestAddressRange{GuestAddressLimit,1,1,0},GuestAddressRange{base,UINT64_MAX,2,0},
   GuestAddressRange{base,1,4,1}}) {
  CHECK(!BuildGuestAddressTable(std::span(&bad,1),&table));CHECK(table==saved);
 }
 std::array overlapping{input[0],input[1],GuestAddressRange{base+8,1,4,0}};
 CHECK(!BuildGuestAddressTable(overlapping,&table));CHECK(table==saved);
 std::vector<GuestAddressRange> huge(GuestAddressMaxRanges+1);
 CHECK(!BuildGuestAddressTable(huge,&table));CHECK(table==saved);
 GuestAddressRange last{GuestAddressLimit-1,UINT64_MAX,1,0};
 CHECK(BuildGuestAddressTable(std::span(&last,1),&table));
 CHECK(ResolveGuestAddress(table,GuestAddressLimit-1,1,&out));CHECK(out==UINT64_MAX);
 CHECK(!ResolveGuestAddress(table,GuestAddressLimit-1,2,&out));
 CHECK(!ResolveGuestAddress(table,UINT64_MAX,2,&out));
 CHECK(BuildGuestAddressTable({},&table));CHECK(table.empty());
 std::puts("PASS: sparse guest address table, discontiguous pages, boundaries, overflow and transactional rejection");
}
