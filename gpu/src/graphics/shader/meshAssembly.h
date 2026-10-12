#pragma once
#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {
// Point-list ES/GS expansion used by the captured Astro shader.
// Logical invocation counts are guest counts, not native subgroup counts.
struct MeshAssemblyPlan {
 uint32_t max_vertices=0, max_primitives=0;
 uint32_t primitives_per_group=0, vertices_per_group=0;
 uint32_t logical_invocations=0, guest_waves=0;
};
inline bool PlanPointMeshAssembly(uint32_t primitive_group, uint32_t vertex_group,
                                 uint32_t vertices_per_primitive, uint32_t max_vertices,
                                 uint32_t wave_size, MeshAssemblyPlan* output) {
 if(output==nullptr || (wave_size!=32 && wave_size!=64) || primitive_group==0 ||
    primitive_group>64 || vertex_group==0 || vertex_group>64 ||
    vertices_per_primitive<3 || vertices_per_primitive>1024 ||
    max_vertices==0 || max_vertices>1024) return false;
 MeshAssemblyPlan plan;
 plan.max_vertices=max_vertices;
 plan.max_primitives=primitive_group*(vertices_per_primitive-2);
 if(plan.max_primitives>1024) return false;
 plan.primitives_per_group=std::min({primitive_group,vertex_group,
                                    max_vertices/vertices_per_primitive});
 if(plan.primitives_per_group==0) return false;
 plan.vertices_per_group=plan.primitives_per_group;
 plan.logical_invocations=((max_vertices+wave_size-1)/wave_size)*wave_size;
 plan.guest_waves=plan.logical_invocations/wave_size;
 // S3 carries total waves in four bits. Native device limits are checked separately.
 if(plan.guest_waves>15) return false;
 *output=plan;
 return true;
}
} // namespace Libs::Graphics
