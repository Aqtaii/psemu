#include "graphics/shader/recompiler/meshOutputProbe.h"
#include "graphics/host_gpu/renderer/guestAddressTable.h"
#include "graphics/host_gpu/renderer/fenceCompletionActions.h"
#include "graphics/host_gpu/renderer/guestAddressFaultRecord.h"
#include <memory>
#include <cstdio>
#include <array>
using namespace Libs::Graphics;
#define CHECK(x) do {if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
 using ShaderRecompiler::MeshOutputProbe;
 MeshOutputProbe probe;
 std::array<float,4> position{9,9,9,9};
 CHECK(!ShaderRecompiler::MeshProbePosition(probe,0,&position));
 CHECK(position[0]==9);
 probe.magic=MeshOutputProbe::Magic;probe.vertices=256;
 probe.lanes[255].position={0xbf800000u,0x40000000u,0x80000000u,0x3f800000u};
 CHECK(!ShaderRecompiler::MeshProbePosition(probe,255,&position));
 probe.lanes[255].seen=1;
 CHECK(ShaderRecompiler::MeshProbePosition(probe,255,&position));
 CHECK(position[0]==-1 && position[1]==2 && position[3]==1);
 CHECK(std::bit_cast<uint32_t>(position[2])==0x80000000u);
 CHECK(!ShaderRecompiler::MeshProbePosition(probe,256,&position));
 probe.vertices=255;CHECK(!ShaderRecompiler::MeshProbePosition(probe,255,&position));
 CHECK(!ShaderRecompiler::MeshProbePosition(probe,0,nullptr));
 CHECK(reinterpret_cast<const char*>(&probe.lanes[0])-reinterpret_cast<const char*>(&probe)==32);
 CHECK(reinterpret_cast<const char*>(&probe.magic)-reinterpret_cast<const char*>(&probe)==16);
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
 // A native buffer may contain both coherent BVH bytes and stale image-owned
 // pixels. Publish only coherent fragments, retaining exact physical offsets.
 const std::array shared{GuestAddressRange{base,0x80000000,32768,0}};
 const std::array cut{GpuWrittenBytes{base+4096,8192}};
 CHECK(ExcludeGuestAddressRanges(shared,cut,&table));
 CHECK(table.size()==2 && table[0].size==4096);
 CHECK(table[1].guest_base==base+12288 && table[1].device_base==0x80003000);
 CHECK(ResolveGuestAddress(table,base+4092,4,&out));
 CHECK(!ResolveGuestAddress(table,base+4092,8,&out));
 CHECK(!ResolveGuestAddress(table,base+4096,4,&out));
 CHECK(ResolveGuestAddress(table,base+12288,4,&out) && out==0x80003000);
 const auto clipped=table;
 const std::array invalid_cut{GpuWrittenBytes{GuestAddressLimit-1,2}};
 CHECK(!ExcludeGuestAddressRanges(shared,invalid_cut,&table) && table==clipped);
 const std::array covers{GpuWrittenBytes{base-1,32770}};
 CHECK(ExcludeGuestAddressRanges(shared,covers,&table) && table.empty());
 const std::array repeated{cut[0],GpuWrittenBytes{base+8192,8192},cut[0]};
 CHECK(ExcludeGuestAddressRanges(shared,repeated,&table));
 CHECK(table.size()==2 && table[1].guest_base==base+16384);
 CHECK(ExcludeGuestAddressRanges(shared,{},&table));
 CHECK(table.size()==1 && table[0]==shared[0]);
 std::puts("PASS: sparse guest address table, discontiguous pages, boundaries, overflow and transactional rejection");
}
