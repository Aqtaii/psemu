#pragma once
#include "graphics/shader/meshAssembly.h"
#include <limits>

namespace Libs::Graphics {
// Initial merged ES/GS registers for a non-indexed point-list draw.
// This is an ABI oracle, not permission to enable the public wave64 emitter.
struct MeshPointDraw {
 uint32_t vertex_count=0, first_vertex=0, first_instance=0;
 uint32_t group_x=0, group_y=0;
 uint64_t gs_user_data_address=0;
};
struct MeshPointInitialRegisters {
 uint32_t s0=0, s1=0, s3=0;
 uint32_t v0=0, v1=0, v5=0, v8=0;
 // ES user data occupies s8 onward; GS s0:s1 is a separate address.
 static constexpr uint32_t es_user_data_base=8;
};
inline bool PlanMeshPointInitialRegisters(const MeshAssemblyPlan& assembly,
                                         uint32_t wave_size, const MeshPointDraw& draw,
                                         uint32_t logical_index,
                                         MeshPointInitialRegisters* output) {
 if(output==nullptr || (wave_size!=32 && wave_size!=64) ||
    assembly.logical_invocations==0 || assembly.logical_invocations%wave_size!=0 ||
    assembly.guest_waves!=assembly.logical_invocations/wave_size ||
    assembly.guest_waves>15 || logical_index>=assembly.logical_invocations ||
    assembly.primitives_per_group==0 || assembly.primitives_per_group>64 ||
    assembly.vertices_per_group!=assembly.primitives_per_group ||
    assembly.max_vertices==0 || assembly.max_vertices>assembly.logical_invocations ||
    assembly.logical_invocations-assembly.max_vertices>=wave_size) return false;
 const uint64_t chunk=uint64_t(draw.group_x)*assembly.primitives_per_group;
 // Keep the draw partition arithmetic representable before forming guest u32 IDs.
 if(chunk>std::numeric_limits<uint32_t>::max()) return false;
 const uint32_t remaining=chunk<draw.vertex_count ? draw.vertex_count-uint32_t(chunk) : 0;
 const uint32_t vertices=std::min(remaining,assembly.vertices_per_group);
 const uint32_t wave_base=logical_index & ~(wave_size-1u);
 const uint32_t count=vertices>wave_base ? std::min(vertices-wave_base,wave_size) : 0;
 MeshPointInitialRegisters result;
 result.s0=uint32_t(draw.gs_user_data_address);
 result.s1=uint32_t(draw.gs_user_data_address>>32u);
 result.s3=(assembly.guest_waves<<28u)|((logical_index/wave_size)<<24u)|(count<<8u)|count;
 result.v0=logical_index<<2u; // point-list adjacency addresses one ES record in LDS
 result.v5=draw.first_vertex+uint32_t(chunk)+logical_index;
 result.v8=draw.first_instance+draw.group_y;
 *output=result;
 return true;
}
} // namespace Libs::Graphics
