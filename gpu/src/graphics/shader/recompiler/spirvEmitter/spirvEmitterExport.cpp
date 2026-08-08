#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitExportComponentF32(EmitterState* state, const IR::Instruction& inst,
                                uint32_t component) {
	const bool enabled = ((inst.export_info.en >> component) & 1u) != 0;
	if (!enabled || component >= inst.src_count || component >= 4u) {
		return ConstantF32(state, component == 3u ? 0x3f800000u : 0u);
	}
	return EmitFloatLoad(state, inst.src[component]);
}

uint32_t EmitExportVec4F32(EmitterState* state, const IR::Instruction& inst) {
	if (inst.export_info.compr) {
		uint32_t components[4] = {
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0x3f800000u),
		};
		for (uint32_t pair_index = 0; pair_index < 2u && pair_index < inst.src_count;
		     pair_index++) {
			const auto raw      = EmitValueLoad(state, inst.src[pair_index]);
			const auto unpacked = state->builder.AllocateId();
			state->builder.AddFunction({OpExtInst, state->vec2_float_type, unpacked,
			                            state->glsl_std450, GlslUnpackHalf2x16, raw});
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair_index * 2u + lane;
				if (((inst.export_info.en >> pair_index) & 1u) == 0) {
					continue;
				}
				components[component] = state->builder.AllocateId();
				state->builder.AddFunction(
				    {OpCompositeExtract, state->float_type, components[component], unpacked, lane});
			}
		}
		const auto vec = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec,
		                            components[0], components[1], components[2], components[3]});
		return vec;
	}

	const auto x   = EmitExportComponentF32(state, inst, 0);
	const auto y   = EmitExportComponentF32(state, inst, 1);
	const auto z   = EmitExportComponentF32(state, inst, 2);
	const auto w   = EmitExportComponentF32(state, inst, 3);
	const auto vec = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, x, y, z, w});
	return vec;
}

uint32_t ApplyMrtExportMapping(EmitterState* state, const IR::Instruction& inst, uint32_t value) {
	if (inst.export_info.kind != IR::ExportTargetKind::Mrt || state->pixel_input_info == nullptr ||
	    inst.export_info.index >= state->pixel_input_info->target_export_mapping.size()) {
		return value;
	}

	const auto mapping = state->pixel_input_info->target_export_mapping[inst.export_info.index];
	if (mapping.IsIdentity()) {
		return value;
	}

	const auto mapped = state->builder.AllocateId();
	state->builder.AddFunction({OpVectorShuffle, state->vec4_float_type, mapped, value, value,
	                            mapping.Map(0), mapping.Map(1), mapping.Map(2), mapping.Map(3)});
	return mapped;
}

bool ExportWritesData(const IR::Instruction& inst) {
	switch (inst.export_info.kind) {
		case IR::ExportTargetKind::Null:
		case IR::ExportTargetKind::Primitive: return false;
		case IR::ExportTargetKind::MrtZ: return (inst.export_info.en & 0x5u) != 0;
		default: return inst.export_info.en != 0;
	}
}

void EmitMrtZExport(EmitterState* state, const IR::Instruction& inst) {
	if ((inst.export_info.en & 0x1u) != 0 && state->depth_variable != 0) {
		state->builder.AddFunction(
		    {OpStore, state->depth_variable, EmitExportComponentF32(state, inst, 0)});
	}

	if ((inst.export_info.en & 0x4u) != 0 && state->sample_mask_variable != 0) {
		const auto raw =
		    inst.src_count > 2u ? EmitValueLoad(state, inst.src[2]) : ConstantU32(state, 0);
		const auto mask = state->builder.AllocateId();
		const auto ptr  = state->builder.AllocateId();
		state->builder.AddFunction({OpBitcast, state->int_type, mask, raw});
		state->builder.AddFunction({OpAccessChain, state->ptr_output_int, ptr,
		                            state->sample_mask_variable, ConstantU32(state, 0)});
		state->builder.AddFunction({OpStore, ptr, mask});
	}
}

// GFX10 NGG primitif disavurumu (exp target=0x14) UC vertex indeksini TEK
// dword'e paketler. Olculen paketleme:
//     v0 = (v0 << 10) | v2
//     v12 = (v1 << 20) | v0        =>  v12 = i0 | (i1 << 10) | (i2 << 20)
// SPIR-V'nin gl_PrimitiveTriangleIndicesEXT'i ise uvec3 ister; birebir
// uymuyor, acmak gerekiyor. Alan genisligi 9 bit (AMD yerlesimi: [8:0],
// [18:10], [28:20]; ust bitler kenar bayraklari). 9 bit 0-511'i kapsiyor,
// MESH_OUTPUT_MAX=256 icin fazlasiyla yeterli - ve 10 bit maskelemek bayrak
// bitini iceri sizdirma riski tasirdi.
static void EmitPrimitiveIndicesExport(EmitterState* state, const IR::Instruction& inst,
                                       uint32_t index) {
	constexpr uint32_t INDEX_MASK = 0x1ffu;
	if (state->mesh_prim_indices_variable == 0 || inst.src_count == 0) {
		return;
	}
	const auto packed = EmitValueLoad(state, inst.src[0]);

	uint32_t components[3] = {};
	for (uint32_t i = 0; i < 3; i++) {
		const auto shifted = state->builder.AllocateId();
		const auto masked  = state->builder.AllocateId();
		state->builder.AddFunction({OpShiftRightLogical, state->uint_type, shifted, packed,
		                            ConstantU32(state, i * 10u)});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, masked, shifted, ConstantU32(state, INDEX_MASK)});
		components[i] = masked;
	}

	const auto vector  = state->builder.AllocateId();
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vector, components[0],
	                            components[1], components[2]});
	state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, pointer,
	                            state->mesh_prim_indices_variable, index});
	state->builder.AddFunction({OpStore, pointer, vector});
}

void EmitExport(EmitterState* state, const IR::Instruction& inst) {
	const bool mesh = state->stage == ShaderType::Mesh;

	if (inst.export_info.kind == IR::ExportTargetKind::Null) {
		return;
	}
	if (inst.export_info.kind == IR::ExportTargetKind::Primitive) {
		// Mesh disinda primitif disavurumu hala anlamsiz: yazacak dizi yok.
		if (mesh) {
			EmitPrimitiveIndicesExport(state, inst, EmitLocalInvocationIndex(state));
		}
		return;
	}

	if (!ExportWritesData(inst)) {
		return;
	}

	if (inst.export_info.kind == IR::ExportTargetKind::MrtZ) {
		EmitMrtZExport(state, inst);
		return;
	}

	const auto variable = OutputVariableForExport(*state, inst.export_info);
	if (variable == 0) {
		return;
	}

	const auto value = ApplyMrtExportMapping(state, inst, EmitExportVec4F32(state, inst));

	// Mesh'te her cikti bir DIZI: cagri kendi lane indeksine yazar. Olculen NGG
	// GS'te bu indeks v13 = (dalga_id << 6) + mbcnt, yani gl_LocalInvocationIndex.
	// Sinir denetimi (v13 < vertex_sayisi) shader'in KENDISINDE exec maskesi
	// olarak zaten var; burada tekrar uretmiyoruz.
	if (inst.export_info.kind == IR::ExportTargetKind::Position) {
		const auto pointer = state->builder.AllocateId();
		if (mesh) {
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer,
			                            variable, EmitLocalInvocationIndex(state),
			                            ConstantU32(state, 0)});
		} else {
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer,
			                            variable, ConstantU32(state, 0)});
		}
		state->builder.AddFunction({OpStore, pointer, value});
		return;
	}

	if (mesh) {
		const auto pointer = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer, variable,
		                            EmitLocalInvocationIndex(state)});
		state->builder.AddFunction({OpStore, pointer, value});
		return;
	}

	state->builder.AddFunction({OpStore, variable, value});
}

bool ExportUsesPixelValidMask(const EmitterState& state, const IR::Instruction& inst) {
	return state.stage == ShaderType::Pixel && inst.export_info.vm && state.needs_pixel_valid_mask;
}

void EmitKillIfBoolFalse(EmitterState* state, uint32_t active) {
	const auto kill_label  = state->builder.AllocateId();
	const auto merge_label = state->builder.AllocateId();
	const auto inactive    = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalNot, state->bool_type, inactive, active});
	state->builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state->builder.AddFunction({OpBranchConditional, inactive, kill_label, merge_label});
	state->builder.AddFunction({OpLabel, kill_label});
	state->builder.AddFunction({OpKill});
	state->builder.AddFunction({OpLabel, merge_label});
}

void EmitUpdatePixelValidMask(EmitterState* state) {
	if (state->pixel_valid_mask_variable == 0) {
		return;
	}

	const auto active = EmitExecActiveBool(state);
	const auto value  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, value, active, ConstantU32(state, 1), ConstantU32(state, 0)});
	state->builder.AddFunction({OpStore, state->pixel_valid_mask_variable, value});
}

void EmitKillIfPixelValidMaskInactive(EmitterState* state) {
	if (state->pixel_valid_mask_variable == 0) {
		return;
	}

	const auto mask_value = state->builder.AllocateId();
	const auto active     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpLoad, state->uint_type, mask_value, state->pixel_valid_mask_variable});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, active, mask_value, ConstantU32(state, 0)});
	EmitKillIfBoolFalse(state, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
