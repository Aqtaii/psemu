#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

void EmitMoveU32(EmitterState* state, const IR::Instruction& inst) {
	const auto value = EmitValueLoad(state, inst.src[0]);
	EmitStoreU32(state, inst.dst, value);
}

void EmitMoveF32Bits(EmitterState* state, const IR::Instruction& inst) {
	const auto value = EmitFloatLoad(state, inst.src[0]);
	const auto bits  = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, value});
	EmitStoreU32(state, inst.dst, bits);
}

uint32_t MoveRelRegisterLimit(const EmitterState& state, IR::Register base) {
	uint32_t limit = base.index + 1u;
	for (const auto& binding: state.registers) {
		if (binding.reg.file == IR::RegisterFile::Vector) {
			limit = std::max(limit, binding.reg.index + 1u);
		}
	}
	return std::min(limit, 256u);
}

void EmitMoveRelSourceU32(EmitterState* state, const IR::Instruction& inst) {
	if (inst.src[0].kind != IR::OperandKind::Register ||
	    inst.src[0].reg.file != IR::RegisterFile::Vector) {
		EmitMoveU32(state, inst);
		return;
	}

	const auto it = state->indexed_vector_arrays.find(&inst);
	if (it != state->indexed_vector_arrays.end()) {
		const auto var        = it->second.variable_id;
		const auto count      = it->second.count;
		const auto array_type = it->second.array_type;
		const auto base_reg   = inst.src[0].reg;
		const auto m0 = inst.src_count > 1u ? EmitValueLoad(state, inst.src[1]) : ConstantU32(state, 0);

		std::vector<uint32_t> compose {OpCompositeConstruct, array_type, state->builder.AllocateId()};
		compose.reserve(3u + count);
		for (uint32_t i = 0; i < count; i++) {
			compose.push_back(EmitRegisterLoad(state, {IR::RegisterFile::Vector, base_reg.index + i}));
		}
		state->builder.AddFunction(compose);
		state->builder.AddFunction({OpStore, var, compose[2]});

		const auto in_range = state->builder.AllocateId();
		state->builder.AddFunction({OpULessThan, state->bool_type, in_range, m0, ConstantU32(state, count)});
		const auto index = state->builder.AllocateId();
		state->builder.AddFunction({OpSelect, state->uint_type, index, in_range, m0, ConstantU32(state, 0)});
		const auto ptr = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_func_uint, ptr, var, index});
		const auto result = state->builder.AllocateId();
		state->builder.AddFunction({OpLoad, state->uint_type, result, ptr});
		EmitStoreU32(state, inst.dst, result);
		return;
	}

	const auto base_reg = inst.src[0].reg;
	const auto m0 = inst.src_count > 1u ? EmitValueLoad(state, inst.src[1]) : ConstantU32(state, 0);
	auto       selected = EmitRegisterLoad(state, base_reg);
	const auto limit    = MoveRelRegisterLimit(*state, base_reg);
	for (uint32_t reg = base_reg.index + 1u; reg < limit; reg++) {
		const auto candidate = EmitRegisterLoad(state, {IR::RegisterFile::Vector, reg});
		const auto match     = state->builder.AllocateId();
		const auto next      = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpIEqual, state->bool_type, match, m0, ConstantU32(state, reg - base_reg.index)});
		state->builder.AddFunction({OpSelect, state->uint_type, next, match, candidate, selected});
		selected = next;
	}
	EmitStoreU32(state, inst.dst, selected);
}

void EmitMoveRelDestU32(EmitterState* state, const IR::Instruction& inst) {
	if (inst.dst.kind != IR::OperandKind::Register ||
	    inst.dst.reg.file != IR::RegisterFile::Vector) {
		EmitMoveU32(state, inst);
		return;
	}

	const auto base_reg = inst.dst.reg;
	const auto value    = EmitValueLoad(state, inst.src[0]);
	const auto m0 = inst.src_count > 1u ? EmitValueLoad(state, inst.src[1]) : ConstantU32(state, 0);
	const auto exec_active = EmitExecActiveBool(state);
	const auto limit       = MoveRelRegisterLimit(*state, base_reg);
	for (uint32_t reg = base_reg.index; reg < limit; reg++) {
		const auto pointer = PointerForRegister(*state, {IR::RegisterFile::Vector, reg});
		if (pointer == 0) {
			continue;
		}
		const auto old_value = state->builder.AllocateId();
		const auto match     = state->builder.AllocateId();
		const auto write     = state->builder.AllocateId();
		const auto merged    = state->builder.AllocateId();
		state->builder.AddFunction({OpLoad, state->uint_type, old_value, pointer});
		state->builder.AddFunction(
		    {OpIEqual, state->bool_type, match, m0, ConstantU32(state, reg - base_reg.index)});
		state->builder.AddFunction({OpLogicalAnd, state->bool_type, write, exec_active, match});
		state->builder.AddFunction({OpSelect, state->uint_type, merged, write, value, old_value});
		state->builder.AddFunction({OpStore, pointer, merged});
	}
}

void EmitMoveU64(EmitterState* state, const IR::Instruction& inst) {
	if (state->per_invocation_masks && inst.dst.kind == IR::OperandKind::Register &&
	    IsMaskRegisterFile(inst.dst.reg.file)) {
		EmitPerInvocationMask(state, inst.dst, EmitLaneMaskOperandActiveBool(state, inst.src[0]));
		return;
	}
	const auto low  = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto high = EmitSequentialValueLoad(state, inst.src[0], 1);
	EmitStoreU32(state, inst.dst, low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), high);
}

uint32_t EmitWqmLaneU32(EmitterState* state, uint32_t src) {
	uint32_t ret = ConstantU32(state, 0);
	for (uint32_t i = 0; i < 8u; i++) {
		const auto mask     = 0x0fu << (i * 4u);
		const auto masked   = state->builder.AllocateId();
		const auto non_zero = state->builder.AllocateId();
		const auto expanded = state->builder.AllocateId();
		const auto combined = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, masked, src, ConstantU32(state, mask)});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, non_zero, masked, ConstantU32(state, 0)});
		state->builder.AddFunction({OpSelect, state->uint_type, expanded, non_zero,
		                            ConstantU32(state, mask), ConstantU32(state, 0)});
		state->builder.AddFunction({OpBitwiseOr, state->uint_type, combined, ret, expanded});
		ret = combined;
	}
	return ret;
}

void EmitWqmB64(EmitterState* state, const IR::Instruction& inst) {
	if (!state->per_invocation_masks && inst.dst.kind == IR::OperandKind::Register &&
	    inst.dst.reg.file == IR::RegisterFile::Scalar) {
		const auto low      = EmitSequentialValueLoad(state, inst.src[0], 0);
		const auto high     = EmitSequentialValueLoad(state, inst.src[0], 1);
		const auto ret_low  = EmitWqmLaneU32(state, low);
		const auto ret_high = EmitWqmLaneU32(state, high);
		EmitStoreU32(state, inst.dst, ret_low);
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
		return;
	}
	const auto ballot = state->builder.AllocateId();
	const auto lane_active =
	    EmitMaskHelperInvocations(state, EmitLaneMaskOperandActiveBool(state, inst.src[0]));
	state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
	                            ConstantU32(state, ScopeSubgroup), lane_active});
	const auto low  = state->builder.AllocateId();
	const auto high = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, low, ballot, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, high, ballot, 1});
	const auto wqm_low   = EmitWqmLaneU32(state, low);
	const auto wqm_high  = EmitWqmLaneU32(state, high);
	const auto lane      = EmitSubgroupLocalInvocationId(state);
	const auto high_half = state->builder.AllocateId();
	const auto mask      = state->builder.AllocateId();
	const auto bit_index = state->builder.AllocateId();
	const auto bit       = state->builder.AllocateId();
	const auto hit       = state->builder.AllocateId();
	const auto active    = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpUGreaterThanEqual, state->bool_type, high_half, lane, ConstantU32(state, 32)});
	state->builder.AddFunction({OpSelect, state->uint_type, mask, high_half, wqm_high, wqm_low});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, bit_index, lane, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, bit, ConstantU32(state, 1), bit_index});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, hit, mask, bit});
	state->builder.AddFunction({OpINotEqual, state->bool_type, active, hit, ConstantU32(state, 0)});
	if (state->per_invocation_masks) {
		EmitPerInvocationMask(state, inst.dst, active);
	} else {
		const auto result = state->builder.AllocateId();
		state->builder.AddFunction({OpSelect, state->uint_type, result, active,
		                            ConstantU32(state, 1), ConstantU32(state, 0)});
		EmitStoreU32(state, inst.dst, result);
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ConstantU32(state, 0));
	}
}

void EmitSaveexecPerInvocation(EmitterState* state, const IR::Instruction& inst) {
	const auto old_exec = EmitExecActiveBool(state);
	const auto src      = EmitLaneMaskOperandActiveBool(state, inst.src[0]);
	EmitPerInvocationMask(state, inst.dst, old_exec);

	uint32_t lhs    = old_exec;
	uint32_t rhs    = src;
	bool     use_or = false;
	switch (inst.saveexec_mode) {
		case IR::SaveexecMode::And: break;
		case IR::SaveexecMode::Andn1: rhs = EmitLogicalNotBool(state, src); break;
		case IR::SaveexecMode::Orn2:
			lhs    = EmitLogicalNotBool(state, old_exec);
			use_or = true;
			break;
	}
	const auto result =
	    use_or ? EmitLogicalOrBool(state, lhs, rhs) : EmitLogicalAndBool(state, lhs, rhs);
	EmitPerInvocationMask(state, MakeRegisterOperand(IR::RegisterFile::Exec, 0), result);
	EmitStoreSccBool(state, result);
}

void EmitSaveexecB32(EmitterState* state, const IR::Instruction& inst) {
	if (state->per_invocation_masks) {
		EmitSaveexecPerInvocation(state, inst);
		return;
	}
	const auto old_low = EmitRegisterLoad(state, {IR::RegisterFile::Exec, 0});
	const auto src_low = EmitValueLoad(state, inst.src[0]);

	EmitStoreU32(state, inst.dst, old_low);

	uint32_t rhs_low = src_low;
	if (inst.saveexec_mode == IR::SaveexecMode::Andn1) {
		rhs_low = state->builder.AllocateId();
		state->builder.AddFunction({OpNot, state->uint_type, rhs_low, src_low});
	}

	const auto new_low = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, new_low, rhs_low, old_low});
	EmitStoreU32(state, MakeRegisterOperand(IR::RegisterFile::Exec, 0), new_low);

	if (state->wave_size == 32u) {
		EmitStoreU32(state, MakeRegisterOperand(IR::RegisterFile::Exec, 1), ConstantU32(state, 0));
	}

	const auto cond = state->builder.AllocateId();
	const auto scc  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, cond, new_low, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, scc, cond, ConstantU32(state, 1), ConstantU32(state, 0)});
	EmitStoreU32(state, SccOperand(), scc);
}

void EmitSaveexecB64(EmitterState* state, const IR::Instruction& inst) {
	if (state->per_invocation_masks) {
		EmitSaveexecPerInvocation(state, inst);
		return;
	}
	const auto old_low  = EmitRegisterLoad(state, {IR::RegisterFile::Exec, 0});
	const auto old_high = EmitRegisterLoad(state, {IR::RegisterFile::Exec, 1});
	const auto src_low  = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto src_high = EmitSequentialValueLoad(state, inst.src[0], 1);

	EmitStoreU32(state, inst.dst, old_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), old_high);

	uint32_t lhs_low  = old_low;
	uint32_t lhs_high = old_high;
	uint32_t rhs_low  = src_low;
	uint32_t rhs_high = src_high;
	uint32_t opcode   = OpBitwiseAnd;
	if (inst.saveexec_mode == IR::SaveexecMode::Andn1) {
		rhs_low  = state->builder.AllocateId();
		rhs_high = state->builder.AllocateId();
		state->builder.AddFunction({OpNot, state->uint_type, rhs_low, src_low});
		state->builder.AddFunction({OpNot, state->uint_type, rhs_high, src_high});
	}
	if (inst.saveexec_mode == IR::SaveexecMode::Orn2) {
		lhs_low  = state->builder.AllocateId();
		lhs_high = state->builder.AllocateId();
		state->builder.AddFunction({OpNot, state->uint_type, lhs_low, old_low});
		state->builder.AddFunction({OpNot, state->uint_type, lhs_high, old_high});
		opcode = OpBitwiseOr;
	}

	const auto new_low  = state->builder.AllocateId();
	const auto new_high = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, new_low, lhs_low, rhs_low});
	state->builder.AddFunction({opcode, state->uint_type, new_high, lhs_high, rhs_high});
	EmitStoreU32(state, MakeRegisterOperand(IR::RegisterFile::Exec, 0), new_low);
	EmitStoreU32(state, MakeRegisterOperand(IR::RegisterFile::Exec, 1), new_high);

	const auto active_new_high = state->wave_size == 32u ? ConstantU32(state, 0) : new_high;
	const auto mask            = state->builder.AllocateId();
	const auto cond            = state->builder.AllocateId();
	const auto scc             = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, mask, new_low, active_new_high});
	state->builder.AddFunction({OpINotEqual, state->bool_type, cond, mask, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, scc, cond, ConstantU32(state, 1), ConstantU32(state, 0)});
	EmitStoreU32(state, SccOperand(), scc);
}

void EmitReadFirstLaneU32(EmitterState* state, const IR::Instruction& inst) {
	const auto src         = EmitValueLoad(state, inst.src[0]);
	const auto active      = EmitMaskHelperInvocations(state, EmitExecActiveBool(state));
	const auto ballot      = state->builder.AllocateId();
	const auto first_lane  = state->builder.AllocateId();
	const auto first_value = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
	                            ConstantU32(state, ScopeSubgroup), active});
	state->builder.AddFunction({OpGroupNonUniformBallotFindLSB, state->uint_type, first_lane,
	                            ConstantU32(state, ScopeSubgroup), ballot});
	state->builder.AddFunction({OpGroupNonUniformShuffle, state->uint_type, first_value,
	                            ConstantU32(state, ScopeSubgroup), src, first_lane});
	EmitStoreU32(state, inst.dst, first_value);
}

uint32_t EmitLaneIndex(EmitterState* state, const IR::Operand& operand) {
	const auto lane = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, lane, EmitValueLoad(state, operand),
	                            ConstantU32(state, 63)});
	return lane;
}

void EmitReadLaneU32(EmitterState* state, const IR::Instruction& inst) {
	const auto src   = EmitValueLoad(state, inst.src[0]);
	const auto lane  = EmitLaneIndex(state, inst.src[1]);
	const auto value = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformShuffle, state->uint_type, value,
	                            ConstantU32(state, ScopeSubgroup), src, lane});
	EmitStoreU32(state, inst.dst, value);
}

void EmitWriteLaneU32(EmitterState* state, const IR::Instruction& inst) {
	const auto value = EmitValueLoad(state, inst.src[0]);
	const auto lane  = EmitLaneIndex(state, inst.src[1]);
	const auto subid = EmitSubgroupLocalInvocationId(state);
	const auto hit   = state->builder.AllocateId();
	const auto pointer =
	    inst.dst.kind == IR::OperandKind::Register ? PointerForRegister(*state, inst.dst.reg) : 0;
	const auto old    = pointer != 0 ? state->builder.AllocateId() : ConstantU32(state, 0);
	const auto merged = state->builder.AllocateId();
	if (pointer != 0) {
		state->builder.AddFunction({OpLoad, state->uint_type, old, pointer});
	}
	state->builder.AddFunction({OpIEqual, state->bool_type, hit, subid, lane});
	state->builder.AddFunction({OpSelect, state->uint_type, merged, hit, value, old});
	if (pointer != 0) {
		// RDNA2 V_WRITELANE_B32 ignores EXEC, unlike ordinary VGPR destination writes.
		state->builder.AddFunction({OpStore, pointer, merged});
	}
}

void EmitPermlaneB32(EmitterState* state, const IR::Instruction& inst, bool x16) {
	const auto subid     = EmitSubgroupLocalInvocationId(state);
	const auto row       = state->builder.AllocateId();
	const auto lane      = state->builder.AllocateId();
	const auto lane8     = state->builder.AllocateId();
	const auto shift     = state->builder.AllocateId();
	const auto upper     = state->builder.AllocateId();
	const auto selected  = state->builder.AllocateId();
	const auto index0    = state->builder.AllocateId();
	const auto index1    = state->builder.AllocateId();
	const auto target    = state->builder.AllocateId();
	const auto shuffled  = state->builder.AllocateId();
	const auto value     = EmitValueLoad(state, inst.src[0]);
	const auto src1      = EmitValueLoad(state, inst.src[1]);
	const auto src2      = EmitValueLoad(state, inst.src[2]);
	uint32_t   row_value = row;
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, row, subid, ConstantU32(state, 0xfffffff0u)});
	if (x16) {
		row_value = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseXor, state->uint_type, row_value, row, ConstantU32(state, 16)});
	}
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 15)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane8, lane, ConstantU32(state, 7)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, shift, lane8, ConstantU32(state, 2)});
	state->builder.AddFunction(
	    {OpUGreaterThanEqual, state->bool_type, upper, lane, ConstantU32(state, 8)});
	state->builder.AddFunction({OpSelect, state->uint_type, selected, upper, src2, src1});
	state->builder.AddFunction({OpShiftRightLogical, state->uint_type, index0, selected, shift});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, index1, index0, ConstantU32(state, 15)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, row_value, index1});
	state->builder.AddFunction({OpGroupNonUniformShuffle, state->uint_type, shuffled,
	                            ConstantU32(state, ScopeSubgroup), value, target});
	uint32_t ret = shuffled;
	if (!inst.dst.op_sel) {
		const auto source_active = EmitLaneIndexActiveBool(state, target);
		ret                      = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, ret, source_active, shuffled, ConstantU32(state, 0)});
	}
	EmitStoreU32(state, inst.dst, ret);
}

void EmitControlNop(EmitterState* state, const IR::Instruction& inst) {
	(void)state;
	(void)inst;
}

void EmitWaitcnt(EmitterState* state, const IR::Instruction& inst) {
	(void)state;
	(void)inst;
}

void EmitBarrier(EmitterState* state, const IR::Instruction& inst) {
	(void)inst;
	const auto semantics = MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory;
	state->builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                            ConstantU32(state, ScopeWorkgroup), ConstantU32(state, semantics)});
}

// NGG GS_ALLOC_REQ (s_sendmsg 0x9) -> OpSetMeshOutputsEXT.
//
// m0 kodlamasi olculdu (bkz. LowerSendmsg):  m0 = vertex | (primitif << 12)
// Iki alan da calisma zamani degeri. OpSetMeshOutputsEXT'in iki operandi da
// resmi gramerde IdRef - yani dinamik SSA degeri kabul ediyor, literal degil
// (C:\VulkanSDK\...\spirv.core.grammar.json'dan dogrulandi). Bu yuzden statik
// cikarim gerekmiyor: bit alanlarini SPIR-V'de ayikliyoruz.
//
// Mesaj kimligi 0x9 disindaki her sey ve Mesh disindaki her asama eskisi gibi
// no-op kalir.
void EmitSendmsg(EmitterState* state, const IR::Instruction& inst) {
	constexpr uint32_t GS_ALLOC_REQ   = 0x9u;
	constexpr uint32_t MESSAGE_ID_BITS = 0xfu;
	constexpr uint32_t COUNT_MASK      = 0xfffu;

	if (state->stage != ShaderType::Mesh || inst.src_count < 2u ||
	    inst.src[0].kind != IR::OperandKind::ImmediateU32 ||
	    (inst.src[0].imm & MESSAGE_ID_BITS) != GS_ALLOC_REQ) {
		// SUPHELI #2'NIN ON KOSULU: OpSetMeshOutputsEXT hic yayilmazsa cikti
		// sayisi sifir kalir ve HICBIR SEY rasterlenmez - ekran siyah olur,
		// üstelik tek bir hata mesaji bile cikmadan. Reddedilen her sendmsg'i
		// SEBEBIYLE birlikte kaydet.
		if (state->stage == ShaderType::Mesh) {
			static std::atomic<uint32_t> s_rej {0};
			if (s_rej.fetch_add(1) < 16) {
				printf("[MS-ALLOC] sendmsg REDDEDILDI (pc=0x%08x) src_count=%u src0_kind=%d "
				       "src0_imm=0x%x\n",
				       inst.pc, inst.src_count,
				       inst.src_count > 0 ? static_cast<int>(inst.src[0].kind) : -1,
				       (inst.src_count > 0 && inst.src[0].kind == IR::OperandKind::ImmediateU32)
				           ? inst.src[0].imm
				           : 0u);
				fflush(stdout);
			}
		}
		EmitControlNop(state, inst);
		return;
	}
	{
		static std::atomic<uint32_t> s_ok {0};
		const auto n = s_ok.fetch_add(1) + 1;
		if (n <= 8) {
			printf("[MS-ALLOC] GS_ALLOC_REQ -> OpSetMeshOutputsEXT #%u (pc=0x%08x)\n", n, inst.pc);
			fflush(stdout);
		}
	}

	// MOD 3: tahsis GIRIS BLOGUNDA kosulsuz yapildi. OpSetMeshOutputsEXT
	// SADECE BIR KEZ cagrilabilir - burada tekrar cagirmak gecersiz olurdu.
	if (MeshFixedPositionMode() >= 3 || MeshProbeBlock() != -1 || MeshProbeLo() >= 0) {
		// PROBE modunda tahsis sondanin blogunda yapiliyor;
		// OpSetMeshOutputsEXT yalnizca BIR KEZ cagrilabilir.
		EmitControlNop(state, inst);
		return;
	}

	// MOD 2: m0'dan cikarim yerine SABIT (3 vertex, 1 primitif). Deneyin
	// amaci m0 sayilarini supheli listesinden cikarmak.
	if (MeshFixedPositionMode() >= 2) {
		state->builder.AddFunction(
		    {OpSetMeshOutputsEXT, ConstantU32(state, 3u), ConstantU32(state, 1u)});
		return;
	}

	const auto m0        = EmitValueLoad(state, inst.src[1]);
	const auto vertices  = state->builder.AllocateId();
	const auto shifted   = state->builder.AllocateId();
	const auto primitives = state->builder.AllocateId();

	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, vertices, m0, ConstantU32(state, COUNT_MASK)});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, shifted, m0, ConstantU32(state, 12u)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, primitives, shifted, ConstantU32(state, COUNT_MASK)});

	// TELEMETRI (`PSEMU_TELEMETRY_MESH=1`): GPU tarafindaki bir skaleri
	// okumanin tek kanali framebuffer. Statik analiz tikandi - SCC zinciri,
	// mbcnt (lo+hi) ve s_cselect lowering'lerinin hepsi donanim semantigine
	// SADIK cikti. Kalan bilinmeyen bir CALISMA ZAMANI degeri: oyunun
	// hesapladigi vertex sayisi.
	//
	// Sonucu ekrana KODLUYORUZ: sabit HELLO ucgeni YALNIZCA
	// `vertices > 0` iken yazilir. Tahsis her iki dalda da yapilir ki
	// OpSetMeshOutputsEXT tekduze ve tam bir kez cagrilsin.
	//
	//   KIRMIZI ucgen -> vertex sayisi > 0, yani m0/vcc zinciri SAGLAM;
	//                    suclu kapinin diger operandi (lane indeksi v13)
	//   SIYAH ekran   -> vertex sayisi GERCEKTEN 0; suc m0/vcc_lo zincirinin
	//                    ilk halkasinda
	if (MeshTelemetryEnabled()) {
		state->builder.AddFunction(
		    {OpSetMeshOutputsEXT, ConstantU32(state, 3u), ConstantU32(state, 1u)});

		// MOD 2: kosulu tamamen kaldir. Ucgen KOSULSUZ yazilir; boylece
		// "sayi sifir mi" ile "blok hic calismiyor mu" ayrilir.
		const bool conditional = MeshTelemetryMode() < 2;
		uint32_t   merge_block = 0;
		if (conditional) {
			const auto nonzero     = state->builder.AllocateId();
			const auto write_block = state->builder.AllocateId();
			merge_block            = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpUGreaterThan, state->bool_type, nonzero, vertices, ConstantU32(state, 0u)});
			state->builder.AddFunction({OpSelectionMerge, merge_block, 0u});
			state->builder.AddFunction({OpBranchConditional, nonzero, write_block, merge_block});
			state->builder.AddFunction({OpLabel, write_block});
		}
		if (state->per_vertex_variable != 0) {
			const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto value   = state->builder.AllocateId();
				const auto pointer = state->builder.AllocateId();
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, value,
				                            ConstantF32Value(state, corners[i][0]),
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer,
				                            state->per_vertex_variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, pointer, value});
			}
		}
		if (state->mesh_prim_indices_variable != 0) {
			const auto vector  = state->builder.AllocateId();
			const auto pointer = state->builder.AllocateId();
			state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vector,
			                            ConstantU32(state, 0), ConstantU32(state, 1),
			                            ConstantU32(state, 2)});
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, pointer,
			                            state->mesh_prim_indices_variable, ConstantU32(state, 0)});
			state->builder.AddFunction({OpStore, pointer, vector});
		}
		if (conditional) {
			state->builder.AddFunction({OpBranch, merge_block});
			state->builder.AddFunction({OpLabel, merge_block});
		}
		{
			static std::atomic<uint32_t> s_t {0};
			if (s_t.fetch_add(1) < 32) {
				printf("[MS-TELEMETRI] mod=%d hello enjekte edildi (pc=0x%08x, per_vertex=%u "
				       "prim_idx=%u)\n",
				       MeshTelemetryMode(), inst.pc, state->per_vertex_variable,
				       state->mesh_prim_indices_variable);
				fflush(stdout);
			}
		}
		return;
	}

	state->builder.AddFunction({OpSetMeshOutputsEXT, vertices, primitives});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
