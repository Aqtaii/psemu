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
// First four words retain GuestAddressFaultRecord. Trailing words are diagnostic only.
struct MeshOutputProbe {
 static constexpr uint32_t Magic=0x4d505242, Capacity=256, HeaderWords=8, LaneWords=6;
 std::array<uint32_t,4> fault{};
 uint32_t magic=0, allocation=0, vertices=0, primitives=0;
 struct Lane {std::array<uint32_t,4> position{};uint32_t primitive=0,seen=0;};
 std::array<Lane,Capacity> lanes{};
};
static_assert(sizeof(MeshOutputProbe::Lane)==24);
static_assert(sizeof(MeshOutputProbe)==(8+256*6)*4);
inline bool MeshProbePosition(const MeshOutputProbe& probe,uint32_t index,
                              std::array<float,4>* result) {
 if(!result || probe.magic!=MeshOutputProbe::Magic || index>=probe.vertices ||
    index>=MeshOutputProbe::Capacity || probe.lanes[index].seen!=1) return false;
 std::array<float,4> value;
 for(uint32_t i=0;i<4;++i)value[i]=std::bit_cast<float>(probe.lanes[index].position[i]);
 *result=value;return true;
}
} // namespace Libs::Graphics::ShaderRecompiler
