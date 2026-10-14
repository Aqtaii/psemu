#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace Libs::Graphics::ShaderRecompiler {
inline bool MeshOutputProbeEnabled() {
 const char* flag=std::getenv("PSEMU_ASTRO_MESH_OUTPUT_PROBE");
 return flag && std::strcmp(flag,"1")==0;
}
// Fixed for one process; changing the selector requires a fresh shader cache/run.
inline uint32_t MeshOutputProbeInstance() {
 static const uint32_t selected=[] {
  const char* value=std::getenv("PSEMU_ASTRO_MESH_PROBE_INSTANCE");
  if(!value || !value[0])return uint32_t(0);
  char* end=nullptr;const auto parsed=std::strtoull(value,&end,0);
  return end && *end==0 && parsed<=UINT32_MAX ? uint32_t(parsed) : uint32_t(0);
 }();
 return selected;
}
inline bool MeshOutputProbeIncludesDraw(uint32_t points,uint32_t instances,
                                       uint32_t first_instance,uint32_t selected) {
 // Instance IDs wrap in the same u32 arithmetic as the actual draw loop.
 return points==1 && instances!=0 && uint32_t(selected-first_instance)<instances;
}
// First four words retain GuestAddressFaultRecord. Trailing words are diagnostic only.
struct MeshOutputProbe {
 static constexpr uint32_t Magic=0x4d505242, Capacity=256, HeaderWords=8, LaneWords=17;
 std::array<uint32_t,4> fault{};
 uint32_t magic=0, allocation=0, vertices=0, primitives=0;
 struct Lane {std::array<uint32_t,4> position{};uint32_t primitive=0,seen=0;
  uint32_t gate_value=0,gate_record=0,gate_seen=0,gate_pc=0;
  std::array<uint32_t,4> gate_descriptor{};std::array<uint32_t,3> gate_xyz{};};
 std::array<Lane,Capacity> lanes{};
};
static_assert(sizeof(MeshOutputProbe::Lane)==68);
static_assert(sizeof(MeshOutputProbe)==(8+256*17)*4);
inline bool MeshProbePosition(const MeshOutputProbe& probe,uint32_t index,
                              std::array<float,4>* result) {
 if(!result || probe.magic!=MeshOutputProbe::Magic || index>=probe.vertices ||
    index>=MeshOutputProbe::Capacity || probe.lanes[index].seen!=1) return false;
 std::array<float,4> value;
 for(uint32_t i=0;i<4;++i)value[i]=std::bit_cast<float>(probe.lanes[index].position[i]);
 *result=value;return true;
}
} // namespace Libs::Graphics::ShaderRecompiler
