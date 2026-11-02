#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
#define EXIT(...) throw std::runtime_error("production guard")
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
struct ShaderBufferResource {
 uint64_t address=0; uint32_t stride=4,records=196608,format=5;
 bool swizzle=false,add_tid=false; uint32_t index_stride=0,packed_stride=4;
 uint64_t Base48() const{return address;} uint32_t Stride() const{return stride;}
 uint32_t NumRecords() const{return records;} uint32_t Format() const{return format;}
 bool SwizzleEnabled() const{return swizzle;} bool AddTid() const{return add_tid;}
 uint32_t IndexStride() const{return index_stride;} uint32_t PackedStride() const{return packed_stride;}
};
template<class T> T DecodeNativeDescriptor(const ShaderBufferResource& value){return value;}
struct Binding {bool read=true,written=false,atomic=false,formatted=false,scalar=false;uint32_t packed_stride=4;};
struct Program {
 struct {std::vector<Binding> buffers;std::vector<int> images,samplers,addresses;} info;
};
struct Resources {std::vector<ShaderBufferResource> buffers;};
struct ShaderComputeInputInfo {
 struct {Program* program;Resources* resources;} stage;
 bool dispatch_thread_dimensions=false,tg_size_en=false;
 uint32_t threads_num[3]={64,1,1},dispatch_threads_num[3]={0,0,0};
 bool group_id[3]={true,false,false}; uint32_t thread_ids_num=1,wave_size=32;
};
namespace HW {
 struct DepthRenderTarget {uint64_t htile_data_base_addr=0;};
 struct Context {DepthRenderTarget depth;const auto& GetDepthRenderTarget() const{return depth;}};
}
struct HtileClearTarget {uint64_t address=0,size=0;};
struct Cache {
 std::vector<HtileClearTarget> ranges;
 int register_calls=0,clear_calls=0;
 bool IsMetaRange(uint64_t a,uint64_t n) const {
  for(auto r:ranges)if(r.address==a && r.size==n)return true;return false;
 }
 struct Region {bool metadata_pages=false;};
 Region QueryRegion(uint64_t a,uint64_t n) const {
  for(auto r:ranges)if(a<r.address+r.size && r.address<a+n)return {true};return {};
 }
 void RegisterMeta(uint64_t a,uint64_t n){++register_calls;ranges.push_back({a,n});}
 bool ClearMeta(uint64_t a){++clear_calls;for(auto r:ranges)if(r.address==a)return true;return false;}
};
struct BufferCache {
 struct Access {uint64_t address,size;bool read,write;};
 std::vector<Access> accesses;
 void ValidateGpuAccess(uint64_t a,uint64_t n,bool r,bool w){accesses.push_back({a,n,r,w});}
};
struct RenderContext {
 Cache cache;BufferCache buffers;
 Cache* GetTextureCache(){return &cache;} BufferCache* GetBufferCache(){return &buffers;}
} *g_render_ctx;
int resolve_calls=0,rejection_logs=0;
bool resolve_allowed=false;
bool ResolveHtileClearTarget(const HW::DepthRenderTarget& z,uint64_t n,HtileClearTarget* out) {
 ++resolve_calls;if(!resolve_allowed)return false;*out={z.htile_data_base_addr,n};return true;
}
template<class... Args> void LogRejectedHtileClear(Args&&...){++rejection_logs;}
#include "htile_clear_routing_production.inc"
struct Fixture {
 static constexpr uint64_t meta=0x1dcb8980000ull,side=0x1dcaa72f4d0ull;
 RenderContext render;Program program;Resources resources;HW::Context ctx;
 ShaderComputeInputInfo input{{&program,&resources}};
 Fixture(){
  g_render_ctx=&render;resolve_calls=rejection_logs=0;resolve_allowed=false;
  ctx.depth.htile_data_base_addr=meta;
  program.info.buffers.resize(2);
  program.info.buffers[0].scalar=true;program.info.buffers[0].packed_stride=16;
  resources.buffers={{.address=side,.stride=16,.records=1,.format=77,.packed_stride=16},
                     {.address=meta}};
 }
 bool Dispatch(uint32_t x=30,uint32_t y=17,uint32_t z=1,uint32_t mode=0x41){
  return TryConsumeComputeMetaClear(input,ctx,x,y,z,mode);
 }
 void NoEffects() const {
  CHECK(resolve_calls==0 && rejection_logs==0);
  CHECK(render.cache.register_calls==0 && render.cache.clear_calls==0 && render.buffers.accesses.empty());
 }
 void MakeClear(){program.info.buffers[1].read=false;program.info.buffers[1].written=true;resolve_allowed=true;}
 template<class F> void Reject(F action){bool rejected=false;try{action();}catch(const std::runtime_error&){rejected=true;}CHECK(rejected);CHECK(render.cache.clear_calls==0);}
};
int main() {
#ifdef EXPECT_OLD_ROUTING
 Fixture f;f.program.info.images.push_back(1);
 f.Reject([&]{f.Dispatch();});
 CHECK(resolve_calls==1 && rejection_logs==1 && f.render.cache.register_calls==0);
 puts("Old routing control: captured read-only compute wrongly enters HTile resolver (reproduced)");
#else
 {Fixture f;f.program.info.images.push_back(1);CHECK(!f.Dispatch());f.NoEffects();}
 {Fixture f;CHECK(!f.Dispatch());f.NoEffects();}
 {Fixture f;f.render.cache.ranges.push_back({Fixture::meta,0xc0000});CHECK(!f.Dispatch());f.NoEffects();}
 {Fixture f;f.resources.buffers[1].address+=0x100000;f.program.info.buffers[1].written=true;
  CHECK(!f.Dispatch());f.NoEffects();}
 {Fixture f;f.MakeClear();CHECK(f.Dispatch(3072,1));CHECK(resolve_calls==1);
  CHECK(f.render.cache.register_calls==1 && f.render.cache.clear_calls==1);
  CHECK(f.render.buffers.accesses.size()==2);
  const auto& a=f.render.buffers.accesses[0];CHECK(a.address==Fixture::meta && a.size==0xc0000 && !a.read && a.write);
  const auto& b=f.render.buffers.accesses[1];CHECK(b.address==Fixture::side && b.size==16 && b.read && !b.write);}
 {Fixture f;f.MakeClear();f.ctx.depth.htile_data_base_addr=0;
  f.render.cache.ranges.push_back({Fixture::meta,0xc0000});CHECK(f.Dispatch(3072,1));
  CHECK(resolve_calls==0 && f.render.cache.register_calls==0 && f.render.cache.clear_calls==1);}
 {Fixture f;f.MakeClear();f.program.info.buffers.push_back(f.program.info.buffers[1]);
  f.resources.buffers.push_back(f.resources.buffers[1]);f.Reject([&]{f.Dispatch(3072,1);});
  CHECK(resolve_calls==0 && f.render.cache.register_calls==0 && f.render.buffers.accesses.empty());}
 {Fixture f;f.MakeClear();f.program.info.buffers[1].atomic=true;f.Reject([&]{f.Dispatch(3072,1);});}
 {Fixture f;f.MakeClear();f.program.info.buffers[1].read=true;f.Reject([&]{f.Dispatch(3072,1);});}
 {Fixture f;f.MakeClear();f.program.info.images.push_back(1);f.Reject([&]{f.Dispatch(3072,1);});}
 {Fixture f;f.MakeClear();f.Reject([&]{f.Dispatch(3071,1);});}
 {Fixture f;f.MakeClear();resolve_allowed=false;f.Reject([&]{f.Dispatch(3072,1);});
  CHECK(rejection_logs==1 && f.render.cache.register_calls==0 && f.render.buffers.accesses.empty());}
 {Fixture f;f.resources.buffers.pop_back();f.Reject([&]{f.Dispatch();});f.NoEffects();}
 puts("HTile routing: 13 production-function read-only, clear, ownership and rejection cases PASS");
#endif
}