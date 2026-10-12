#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"

#include <vector>
#include "graphics/shader/meshAssembly.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv {

bool EmitMergedPointMeshProgram(const IR::Program&, const IR::ResourceSnapshot&,
 const ShaderVertexInputInfo&, const MeshAssemblyPlan&, std::vector<uint32_t>*, std::string*);

bool ProgramRequiresExactSubgroupSize(const IR::Program& program);

bool EmitProgram(const IR::Program& program, const IR::ResourceSnapshot& resources,
                 const ShaderVertexInputInfo*  vertex_input_info,
                 const ShaderPixelInputInfo*   pixel_input_info,
                 const ShaderComputeInputInfo* compute_input_info, std::vector<uint32_t>* spirv,
                 std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
