#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

void ComputeReachableBlocks(EmitterState* state, const IR::Program& program) {
	state->reachable_blocks.assign(program.blocks.size(), false);
	if (program.blocks.empty()) {
		return;
	}

	std::vector<uint32_t> stack;
	stack.push_back(0);
	while (!stack.empty()) {
		const auto block_id = stack.back();
		stack.pop_back();
		if (block_id >= program.blocks.size() || state->reachable_blocks[block_id]) {
			continue;
		}
		state->reachable_blocks[block_id] = true;
		for (auto succ: program.blocks[block_id].successors) {
			if (succ < program.blocks.size() && !state->reachable_blocks[succ]) {
				stack.push_back(succ);
			}
		}
	}
}

void AllocateBlockLabels(EmitterState* state, const IR::Program& program) {
	for (const auto& block: program.blocks) {
		if (block.id < state->reachable_blocks.size() && state->reachable_blocks[block.id]) {
			state->block_labels.emplace(block.id, state->builder.AllocateId());
		}
	}
}

void AllocateDispatcherState(EmitterState* state, const IR::Program& program) {
	state->dispatcher_fallback = program.dispatcher_fallback;
	if (!state->dispatcher_fallback) {
		return;
	}
	state->dispatch_pc_variable        = state->builder.AllocateId();
	state->dispatch_header_label       = state->builder.AllocateId();
	state->dispatch_select_label       = state->builder.AllocateId();
	state->dispatch_default_label      = state->builder.AllocateId();
	state->dispatch_after_switch_label = state->builder.AllocateId();
	state->dispatch_continue_label     = state->builder.AllocateId();
	state->dispatch_merge_label        = state->builder.AllocateId();
}

uint32_t BlockLabel(const EmitterState& state, uint32_t block_id) {
	auto it = state.block_labels.find(block_id);
	return it == state.block_labels.end() ? 0 : it->second;
}

uint32_t EmitBranchCondition(EmitterState* state, CFG::BranchCondition condition) {
	switch (condition) {
		case CFG::BranchCondition::SccZero: return EmitSccBool(state, false);
		case CFG::BranchCondition::SccNonZero: return EmitSccBool(state, true);
		case CFG::BranchCondition::VccZero:
			return EmitMaskSummaryZeroBool(state, IR::RegisterFile::Vcc, true);
		case CFG::BranchCondition::VccNonZero:
			return EmitMaskSummaryZeroBool(state, IR::RegisterFile::Vcc, false);
		case CFG::BranchCondition::ExecZero:
			return EmitMaskSummaryZeroBool(state, IR::RegisterFile::Exec, true);
		case CFG::BranchCondition::ExecNonZero:
			return EmitMaskSummaryZeroBool(state, IR::RegisterFile::Exec, false);
		default: return EmitSccBool(state, true);
	}
}

void EmitStructuredPrefix(EmitterState* state, const CFG::Terminator& term) {
	if (term.loop_header) {
		const auto merge_label    = BlockLabel(*state, term.merge_block);
		const auto continue_label = BlockLabel(*state, term.continue_block);
		if (merge_label != 0 && continue_label != 0) {
			state->builder.AddFunction({OpLoopMerge, merge_label, continue_label, LoopControlNone});
		}
		return;
	}
	// SONDA: merge HIC ATANMAMIS kosullu dallanma. Bu durumda asagidaki blok
	// hic calismiyor, OpSelectionMerge basilmiyor ve spirv-val "Selection must
	// be structured" diyor. Merge'u atanmadan birakan tek yer LATCH DISLAMASI
	// (bu oturumda genisletildi) - gercek bir secim yanlislikla latch
	// sayiliyorsa hata buradan cikar.
	if (term.kind == CFG::TerminatorKind::ConditionalBranch && term.merge_block == UINT32_MAX) {
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 32) {
			printf("[MERGE-ATANMAMIS] kosullu dallanma, merge=YOK | dogru=%u yanlis=%u "
			       "dongu_basligi=%d\n",
			       term.true_block, term.false_block, term.loop_header ? 1 : 0);
			fflush(stdout);
		}
	}
	if (term.kind == CFG::TerminatorKind::ConditionalBranch && term.merge_block != UINT32_MAX) {
		const auto merge_label = BlockLabel(*state, term.merge_block);
		if (merge_label != 0) {
			state->builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
		} else {
			// SONDA: etiket 0 = merge blogu YAYILMAMIS (ulasilamaz sayilip
			// budanmis ya da block_labels'a hic girmemis). Bu durumda
			// OpSelectionMerge HIC basilmiyor ve spirv-val "Selection must be
			// structured" diyor. Sessizce gecmek, hatayi gizliyordu.
			const bool reachable = term.merge_block < state->reachable_blocks.size() &&
			                       state->reachable_blocks[term.merge_block];
			printf("[MERGE-KAYIP] merge=%u etiket ALAMADI | ulasilabilir=%d "
			       "(reachable_blocks boyut=%zu) dogru=%u yanlis=%u\n",
			       term.merge_block, reachable ? 1 : 0, state->reachable_blocks.size(),
			       term.true_block, term.false_block);
			fflush(stdout);
		}
	}
}

void EmitReturn(EmitterState* state) {
	EmitKillIfPixelValidMaskInactive(state);
	state->builder.AddFunction({OpReturn});
}

void EmitTerminator(EmitterState* state, const CFG::Terminator& term) {
	const auto condition = term.kind == CFG::TerminatorKind::ConditionalBranch
	                           ? EmitBranchCondition(state, term.condition)
	                           : 0;
	EmitStructuredPrefix(state, term);

	switch (term.kind) {
		case CFG::TerminatorKind::Branch: {
			const auto label = BlockLabel(*state, term.true_block);
			if (label != 0) {
				state->builder.AddFunction({OpBranch, label});
			} else {
				EmitReturn(state);
			}
			break;
		}
		case CFG::TerminatorKind::ConditionalBranch: {
			const auto true_label  = BlockLabel(*state, term.true_block);
			const auto false_label = BlockLabel(*state, term.false_block);
			if (true_label != 0 && false_label != 0) {
				state->builder.AddFunction(
				    {OpBranchConditional, condition, true_label, false_label});
			} else {
				EmitReturn(state);
			}
			break;
		}
		default: EmitReturn(state); break;
	}
}

bool UserDataDwordIndex(const EmitterState& state, IR::Register reg, uint32_t& dword_index) {
	if (state.program == nullptr || reg.file != IR::RegisterFile::Scalar) {
		return false;
	}
	const auto& registers = state.program->bindings.user_data_registers;
	const auto  found     = std::lower_bound(registers.begin(), registers.end(), reg.index);
	if (found != registers.end() && *found == reg.index) {
		dword_index = static_cast<uint32_t>(found - registers.begin());
		return true;
	}
	return false;
}

uint32_t EmitVsharpDwordLoad(EmitterState* state, uint32_t dword_index) {
	if (state->push_constant_variable != 0) {
		const auto pointer = state->builder.AllocateId();
		const auto value   = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_push_constant_uint, pointer,
		                            state->push_constant_variable, ConstantU32(state, 0),
		                            ConstantU32(state, dword_index / 4u),
		                            ConstantU32(state, dword_index % 4u)});
		state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
		return value;
	}
	if (state->vsharp_storage_variable != 0) {
		const auto pointer = state->builder.AllocateId();
		const auto value   = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_storage_buffer_uint, pointer,
		                            state->vsharp_storage_variable, ConstantU32(state, 0),
		                            ConstantU32(state, dword_index)});
		state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
		return value;
	}
	return ConstantU32(state, 0);
}

uint32_t InitialRegisterValue(const EmitterState& state, IR::Register reg) {
	if (reg.file == IR::RegisterFile::Exec) {
		if (state.per_invocation_masks) {
			return reg.index == 0 ? 1u : 0u;
		}
		if (reg.index == 0) {
			return 0xffffffffu;
		}
		return state.wave_size == 64u ? 0xffffffffu : 0u;
	}
	return 0;
}

uint32_t EmitInitialRegisterValue(EmitterState* state, IR::Register reg) {
	uint32_t dword_index = 0;
	if (UserDataDwordIndex(*state, reg, dword_index)) {
		return EmitVsharpDwordLoad(state, dword_index);
	}
	// NGG SISTEM SGPR PAYLOAD'U (`PSEMU_NGG_SGPR=1`).
	//
	// Donanimda SPI, `user_sgpr` sayisinin ARDINDAN gelen yazmaclari sistem
	// degerleriyle doldurur. psemu'da bu HIC yapilmiyordu: user data
	// penceresi disindaki her skaler 0 kaliyordu (bkz. d19f676).
	//
	// s3'un 27:24 biti = GS dalga kimligi. Olculdu (6d3f2cb): bu alan HEM
	// `GS_ALLOC_REQ` kapisini (`s3 & 0x0f000000`) HEM de LDS adresini
	// (`s2 = bfe(s3,24,4)` -> `v24 = (s2<<6)+mbcnt` -> `v8 = 48*v24`) suruyor.
	//
	// `s2`YE DOKUNULMUYOR: shader onu zaten `bfe(s3,24,4)` ile YENIDEN
	// hesapliyor (dokum satir 527), yani baslangic degeri uzerine yaziliyor.
	// s3 dogruysa s2 kendiliginden dogru olur.
	if (NggSystemSgprEnabled() && state->stage == ShaderType::Mesh &&
	    reg.file == IR::RegisterFile::Scalar && reg.index == 3) {
		const auto wave = state->wave_size != 0 ? state->wave_size : 64u;
		const auto lane = EmitLocalInvocationIndex(state);
		const auto id   = state->builder.AllocateId();
		const auto shifted = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpUDiv, state->uint_type, id, lane, ConstantU32(state, wave)});
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, shifted, id, ConstantU32(state, 24u)});
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 16) {
			std::printf("[NGG-SGPR] s3 = (LocalInvocationIndex / %u) << 24  (GS dalga kimligi)\n",
			            wave);
			std::fflush(stdout);
		}
		return shifted;
	}
	return ConstantU32(state, InitialRegisterValue(*state, reg));
}

// IKILI ARAMA SONDASI: verilen blogun basina KOSULSUZ hello ucgeni + tahsis
// koyar. Tahsis burada yapildigi icin cagiran, oyunun kendi GS_ALLOC_REQ'ini
// no-op'a cevirmelidir (OpSetMeshOutputsEXT yalnizca BIR KEZ cagrilabilir).
void EmitMeshProbeTriangle(EmitterState* state, uint32_t block_id, uint32_t ordinal,
                           bool with_alloc) {
	// Tahsis GIRIS BLOGUNDA bir kez yapilir; burada yalnizca YAZIMLAR olur.
	// Boylece ayni yazim TUM bloklara konulabilir (-2 kipi) - tahsis tekrari
	// gecersiz olurdu.
	if (with_alloc) {
		state->builder.AddFunction(
		    {OpSetMeshOutputsEXT, ConstantU32(state, 3u), ConstantU32(state, 1u)});
	}
	// KOSULLU KIP (`PSEMU_MESH_PROBE_COND=1`): ucgeni yalnizca oyunun KENDI
	// hesapladigi vertex sayisi > 0 ise yaz. Sayiyi `m0`dan turetiyoruz
	// (m0 = vertex | primitif<<12) - yeni degisken gerekmiyor, m0 zaten
	// kalici bir yazmac.
	//
	// Bu deney DAHA ONCE (6514eea) tahsis noktasinda kosulmustu ve yaziminin
	// UZERINE oyunun POS0'i yazdigi icin sonucu ANLAMSIZDI. Artik yazimin
	// hayatta kaldigi blok araligini biliyoruz (69..72), yani ayni soru
	// nihayet dogru yerde sorulabiliyor:
	//   KIRMIZI -> sayi > 0; kusur lane kapisinda ya da pozisyonlarda
	//   SIYAH   -> sayi GERCEKTEN 0; kusur m0 zincirinin kendisinde
	uint32_t cond_merge = 0;
	if (MeshProbeCond()) {
		const auto m0_pointer = PointerForRegister(*state, {IR::RegisterFile::M0, 0});
		if (m0_pointer != 0) {
			const auto m0_value  = state->builder.AllocateId();
			const auto vertices  = state->builder.AllocateId();
			const auto nonzero   = state->builder.AllocateId();
			const auto write_blk = state->builder.AllocateId();
			cond_merge           = state->builder.AllocateId();
			state->builder.AddFunction({OpLoad, state->uint_type, m0_value, m0_pointer});
			state->builder.AddFunction({OpBitwiseAnd, state->uint_type, vertices, m0_value,
			                            ConstantU32(state, 0xfffu)});
			state->builder.AddFunction(
			    {OpUGreaterThan, state->bool_type, nonzero, vertices, ConstantU32(state, 0u)});
			state->builder.AddFunction({OpSelectionMerge, cond_merge, 0u});
			state->builder.AddFunction({OpBranchConditional, nonzero, write_blk, cond_merge});
			state->builder.AddFunction({OpLabel, write_blk});
		}
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
	if (cond_merge != 0) {
		state->builder.AddFunction({OpBranch, cond_merge});
		state->builder.AddFunction({OpLabel, cond_merge});
	}
	static std::atomic<uint32_t> s_n {0};
	if (s_n.fetch_add(1) < 32) {
		std::printf("[MS-PROBE] ucgen blok sirasi=%u (ic id=%u) basina KOSULSUZ yazildi\n", ordinal,
		            block_id);
		std::fflush(stdout);
	}
}

void EmitRegisterVariables(EmitterState* state) {
	if (state->needs_function_lds) {
		state->builder.AddFunction(
		    {OpVariable, state->ptr_workgroup_array, state->lds_variable, StorageClassFunction});
	}
	for (const auto& binding: state->registers) {
		state->builder.AddFunction(
		    {OpVariable, state->ptr_func_uint, binding.pointer_id, StorageClassFunction});
	}
	if (state->dispatcher_fallback) {
		state->builder.AddFunction(
		    {OpVariable, state->ptr_func_uint, state->dispatch_pc_variable, StorageClassFunction});
	}
	if (state->pixel_valid_mask_variable != 0) {
		state->builder.AddFunction({OpVariable, state->ptr_func_uint,
		                            state->pixel_valid_mask_variable, StorageClassFunction});
		state->builder.AddFunction(
		    {OpStore, state->pixel_valid_mask_variable, ConstantU32(state, 1)});
	}
	for (const auto& binding: state->registers) {
		state->builder.AddFunction(
		    {OpStore, binding.pointer_id, EmitInitialRegisterValue(state, binding.reg)});
	}

	// NGG LDS RING BOOKKEEPING BASLANGICI (`PSEMU_NGG_SGPR=1`).
	// ES kendi yazma tabanini `lds_dwords[(160 + v8) >> 2]`den OKUYOR
	// (6d3f2cb). O alan hic kurulmuyorsa devir kopar. Burada giris blogunda
	// sifirlaniyor.
	// NOT: olculdu ki o bolge ZATEN sifir okunuyor (e19ab3c), dolayisiyla bu
	// adimin ETKISIZ kalmasi bekleniyor; dogru duzeltme oraya GERCEK ring
	// tabanini yazmaktir - degeri henuz bilinmiyor. Yine de sozlesmenin
	// parcasi olarak kuruldu ve olcumle ayirt edilebilir.
	if (NggSystemSgprEnabled() && state->stage == ShaderType::Mesh &&
	    state->lds_variable != 0 && state->ptr_workgroup_uint != 0) {
		const auto slot = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_workgroup_uint, slot,
		                            state->lds_variable, ConstantU32(state, 160u / 4u)});
		state->builder.AddFunction({OpStore, slot, ConstantU32(state, 0)});
	}

	// NGG DALGA BILGISI ENJEKSIYONU (`PSEMU_NGG_WAVEINFO_SGPR=<n>`).
	//
	// Olculdu (PSEMU_TELEMETRY_MESH=2): GS_ALLOC_REQ blogu HIC calismiyor -
	// kosulsuz yazim bile siyah, ayni yazimlar giris blogunda KIRMIZI.
	// Kosul `s_and_b32 vcc_lo, s3, 0x0f000000` + `s_cbranch_scc1`; uretilen
	// modulde s3 push constant'tan (user data) geliyor. Donanimda NGG dalga
	// bilgisi user data DEGIL, `user_sgpr` sayisinin ARDINDAN gelen bir
	// SISTEM SGPR'idir - ve olculdu ki gercek user_sgpr 0/4/8 olabiliyorken
	// psemu 16'lik bir taban zorluyor ([NGG-USERSGPR]).
	//
	// NGG payload'un TAM yerlesimini bilmiyoruz. Bu yuzden burasi bir
	// DUZELTME DEGIL, HIPOTEZ TESTI: verilen yazmaca dalga-bilgisi bitlerini
	// (0x0f000000) OR'la ve blogun calisip calismadigina bak.
	//   ucgen/geometri cikarsa -> kapi GERCEKTEN bu SGPR; dogru NGG payload
	//                             emulasyonu yazilmali
	//   siyah kalirsa          -> kapi baska bir girdide
	if (state->stage == ShaderType::Mesh) {
		static const int wave_info_sgpr = [] {
			const char* env = std::getenv("PSEMU_NGG_WAVEINFO_SGPR");
			return env != nullptr ? std::atoi(env) : -1;
		}();
		if (wave_info_sgpr >= 0) {
			const auto pointer = PointerForRegister(
			    *state, {IR::RegisterFile::Scalar, static_cast<uint32_t>(wave_info_sgpr)});
			if (pointer != 0) {
				// POLARITE DOKUMDEN OKUNDU, TAHMIN EDILMEDI:
				//   8814  vcc_lo = s3 & 0x0f000000
				//   8835  OpBranchConditional (vcc_lo != 0) -> ATLA / tahsis
				// Yani tahsis, bu bitler SIFIRKEN calisiyor. Ilk denemede
				// bitleri KURMUSTUM (OR) - bu atlamayi garantiledi ve ekran
				// siyah kaldi. Kod icindeki not ("s_and_b32 + s_cbranch_scc1")
				// kosulun TUTMASI gerektigi izlenimi veriyordu; dal polaritesi
				// tersiymis.
				const auto old_value = state->builder.AllocateId();
				const auto masked    = state->builder.AllocateId();
				state->builder.AddFunction({OpLoad, state->uint_type, old_value, pointer});
				state->builder.AddFunction({OpBitwiseAnd, state->uint_type, masked, old_value,
				                            ConstantU32(state, ~0x0f000000u)});
				state->builder.AddFunction({OpStore, pointer, masked});
				static std::atomic<uint32_t> s_n {0};
				if (s_n.fetch_add(1) < 32) {
					std::printf("[NGG-WAVEINFO] s%d &= ~0x0f000000 (tahsis bitleri TEMIZLENDI)\n",
					            wave_info_sgpr);
					std::fflush(stdout);
				}
			} else {
				static std::atomic<uint32_t> s_m {0};
				if (s_m.fetch_add(1) < 8) {
					std::printf("[NGG-WAVEINFO] s%d bu shader'da YOK - enjeksiyon atlandi\n",
					            wave_info_sgpr);
					std::fflush(stdout);
				}
			}
		}
	}
}

void EmitComputeInputRegisters(EmitterState* state) {
	if (state->stage != ShaderType::Compute || state->compute_input_info == nullptr) {
		return;
	}

	const auto* cs         = state->compute_input_info;
	const auto  thread_ids = cs->thread_ids_num > 0
	                             ? std::min<uint32_t>(static_cast<uint32_t>(cs->thread_ids_num), 3u)
	                             : 0u;
	for (uint32_t i = 0; i < thread_ids; i++) {
		const auto pointer = PointerForRegister(*state, {IR::RegisterFile::Vector, i});
		if (pointer != 0) {
			state->builder.AddFunction(
			    {OpStore, pointer,
			     EmitInputComponentU32(state, IR::StageInputKind::LocalInvocationId, i)});
		}
	}

	uint32_t reg_offset = 0;
	for (uint32_t i = 0; i < 3u; i++) {
		if (!cs->group_id[i]) {
			continue;
		}
		const auto pointer = PointerForRegister(
		    *state,
		    {IR::RegisterFile::Scalar, static_cast<uint32_t>(cs->workgroup_register) + reg_offset});
		if (pointer != 0) {
			state->builder.AddFunction(
			    {OpStore, pointer,
			     EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, i)});
		}
		reg_offset++;
	}

	if (cs->tg_size_en) {
		const auto pointer = PointerForRegister(
		    *state,
		    {IR::RegisterFile::Scalar, static_cast<uint32_t>(cs->workgroup_register) + reg_offset});
		if (pointer != 0) {
			const uint32_t wave_size     = cs->wave_size != 0 ? cs->wave_size : 64u;
			const uint32_t total_threads = std::max<uint32_t>(cs->threads_num[0], 1u) *
			                               std::max<uint32_t>(cs->threads_num[1], 1u) *
			                               std::max<uint32_t>(cs->threads_num[2], 1u);
			const uint32_t waves =
			    std::min<uint32_t>((total_threads + wave_size - 1u) / wave_size, 0x3fu);

			const auto local_index = EmitLocalInvocationIndex(state);
			const auto wave_id     = state->builder.AllocateId();
			const auto wave_bits   = state->builder.AllocateId();
			const auto is_first    = state->builder.AllocateId();
			const auto first_bit   = state->builder.AllocateId();
			const auto base        = state->builder.AllocateId();
			const auto packed      = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpUDiv, state->uint_type, wave_id, local_index, ConstantU32(state, wave_size)});
			state->builder.AddFunction(
			    {OpShiftLeftLogical, state->uint_type, wave_bits, wave_id, ConstantU32(state, 20)});
			state->builder.AddFunction(
			    {OpIEqual, state->bool_type, is_first, wave_id, ConstantU32(state, 0)});
			state->builder.AddFunction({OpSelect, state->uint_type, first_bit, is_first,
			                            ConstantU32(state, 0x80000000u), ConstantU32(state, 0)});
			state->builder.AddFunction(
			    {OpBitwiseOr, state->uint_type, base, wave_bits, ConstantU32(state, waves)});
			state->builder.AddFunction({OpBitwiseOr, state->uint_type, packed, base, first_bit});
			state->builder.AddFunction({OpStore, pointer, packed});
		}
	}
}

void EmitPixelFragCoordComponent(EmitterState* state, uint32_t component, uint32_t vgpr) {
	const auto input = InputVariableForKind(*state, IR::StageInputKind::FragCoord);
	const auto dst   = PointerForRegister(*state, {IR::RegisterFile::Vector, vgpr});
	if (input == 0 || dst == 0) {
		return;
	}

	const auto pointer = state->builder.AllocateId();
	const auto value   = state->builder.AllocateId();
	const auto bits    = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpAccessChain, state->ptr_input_float, pointer, input, ConstantU32(state, component)});
	state->builder.AddFunction({OpLoad, state->float_type, value, pointer});
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, value});
	state->builder.AddFunction({OpStore, dst, bits});
}

void EmitPixelFrontFacingRegister(EmitterState* state, uint32_t vgpr) {
	const auto input = InputVariableForKind(*state, IR::StageInputKind::FrontFacing);
	const auto dst   = PointerForRegister(*state, {IR::RegisterFile::Vector, vgpr});
	if (input == 0 || dst == 0) {
		return;
	}

	const auto value = state->builder.AllocateId();
	const auto bits  = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->bool_type, value, input});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, bits, value, ConstantU32(state, 1), ConstantU32(state, 0)});
	state->builder.AddFunction({OpStore, dst, bits});
}

void EmitPixelInputRegisters(EmitterState* state) {
	if (state->stage != ShaderType::Pixel || state->pixel_input_info == nullptr) {
		return;
	}

	const auto* ps  = state->pixel_input_info;
	uint32_t    reg = ps->ps_system_input_base;
	if (ps->HasPositionInput()) {
		if (ps->ps_pos_x) {
			EmitPixelFragCoordComponent(state, 0, reg);
			reg++;
		}
		if (ps->ps_pos_y) {
			EmitPixelFragCoordComponent(state, 1, reg);
			reg++;
		}
		if (ps->ps_pos_z) {
			EmitPixelFragCoordComponent(state, 2, reg);
			reg++;
		}
		if (ps->ps_pos_w) {
			EmitPixelFragCoordComponent(state, 3, reg);
			reg++;
		}
	}
	if (ps->ps_front_face) {
		EmitPixelFrontFacingRegister(state, reg);
	}
}

uint32_t EmitInputScalarU32(EmitterState* state, IR::StageInputKind kind) {
	const auto variable = InputVariableForKind(*state, kind);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto raw  = state->builder.AllocateId();
	const auto bits = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->int_type, raw, variable});
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, raw});
	return bits;
}

void EmitVertexInputRegisters(EmitterState* state) {
	if (state->stage != ShaderType::Vertex) {
		return;
	}

	const auto vertex_index = PointerForRegister(*state, {IR::RegisterFile::Vector, 5});
	if (vertex_index != 0) {
		state->builder.AddFunction(
		    {OpStore, vertex_index, EmitInputScalarU32(state, IR::StageInputKind::VertexIndex)});
	}

	const auto instance_index = PointerForRegister(*state, {IR::RegisterFile::Vector, 8});
	if (instance_index != 0) {
		state->builder.AddFunction({OpStore, instance_index,
		                            EmitInputScalarU32(state, IR::StageInputKind::InstanceIndex)});
	}
}

// KIRPMA DENEYI (PSEMU_MESH_NOOP). Bisection kanitladi ki device lost
// recompile edilen koddan geliyor (bkz. commit 0687e32). Aramayi daraltmak
// icin gercek shader'in blok blok iptal edilmesi:
//   PSEMU_MESH_NOOP=lds-yaz  -> tum LDS yazmalari atlanir
//   PSEMU_MESH_NOOP=bariyer  -> s_barrier'lar atlanir
//   PSEMU_MESH_NOOP=ikisi    -> ikisi birden
// Yalnizca Mesh asamasinda etkili; diger asamalar dokunulmadan kalir.
static bool MeshNoopSkips(const EmitterState& state, IR::Opcode op) {
	if (state.stage != ShaderType::Mesh) {
		return false;
	}
	const int mode = MeshNoopMode();
	if (mode == 0) {
		return false;
	}
	const bool skip_lds_write = (mode & 1) != 0;
	const bool skip_barrier   = (mode & 2) != 0;
	// "tampon": descriptor uzerinden yapilan TUM bellek okumalari atlanir.
	// Bozuk bir descriptor (yanlis VkBuffer/offset ya da hic yazilmamis giris)
	// tam da BEKLEME=0 ms'lik anlik device lost uretir.
	const bool skip_buffer = (mode & 8) != 0;
	// "hepsi-bellek": descriptor cozumleme (LoadSrtDword), LDS OKUMALARI ve
	// goruntu erisimleri dahil TUM bellek islemleri. Onceki "tampon" denemesi
	// bunlari KACIRMISTI - skaler descriptor cekimi ayri bir opcode.
	const bool skip_memory = (mode & 16) != 0;
	// Son bolme: descriptor cozumleme (srt) mi, LDS okumalari (lds-oku) mi?
	const bool skip_srt      = (mode & 32) != 0;
	const bool skip_lds_read = (mode & 64) != 0;
	// "duz": HAM ADRESLI erisimler (flat/global). Bagli descriptor'lardan
	// gecmeyen tek yol bunlar; robustBufferAccess de korumaz. LoadSrtDword'un
	// okudugu dword'ler burada isaretci olarak kullaniliyorsa, page fault'un
	// gercek cikis noktasi burasidir.
	const bool skip_flat = (mode & 128) != 0;
	switch (op) {
		case IR::Opcode::LoadSrtDword: return skip_memory || skip_srt;
		case IR::Opcode::DsReadUbyte:
		case IR::Opcode::DsReadSbyte:
		case IR::Opcode::DsReadUshort:
		case IR::Opcode::DsReadSshort:
		case IR::Opcode::DsReadB32:
		case IR::Opcode::DsReadAddtidB32: return skip_memory || skip_lds_read;
		case IR::Opcode::ImageSample:
		case IR::Opcode::ImageGather4:
		case IR::Opcode::ImageGetLod:
		case IR::Opcode::ImageLoad:
		case IR::Opcode::ImageStore:
		case IR::Opcode::ImageGetResinfo: return skip_memory;
		case IR::Opcode::SLoadDword:
		case IR::Opcode::SBufferLoadDword:
		case IR::Opcode::BufferLoadUbyte:
		case IR::Opcode::BufferLoadSbyte:
		case IR::Opcode::BufferLoadUshort:
		case IR::Opcode::BufferLoadSshort:
		case IR::Opcode::BufferLoadDword:
		case IR::Opcode::FlatLoadUbyte:
		case IR::Opcode::FlatLoadSbyte:
		case IR::Opcode::FlatLoadUshort:
		case IR::Opcode::FlatLoadSshort:
		case IR::Opcode::FlatLoadDword:
		case IR::Opcode::FlatStoreByte:
		case IR::Opcode::FlatStoreShort:
		case IR::Opcode::FlatStoreDword: return skip_buffer || skip_memory || skip_flat;
		case IR::Opcode::DsWriteByte:
		case IR::Opcode::DsWriteShort:
		case IR::Opcode::DsWriteB32:
		case IR::Opcode::DsWriteAddtidB32:
		case IR::Opcode::DsMinF32:
		case IR::Opcode::DsMaxF32: return skip_lds_write || skip_memory;
		case IR::Opcode::Barrier: return skip_barrier;
		default: return false;
	}
}

void EmitInstruction(EmitterState* state, const IR::Instruction& inst) {
	if (MeshNoopSkips(*state, inst.op)) {
		static std::atomic<uint64_t> s_n {0};
		const auto                   n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n == 1 || (n % 256) == 0) {
			printf("[MESH-NOOP] atlanan komut sayisi=%llu\n", (unsigned long long)n);
			fflush(stdout);
		}
		return;
	}
	switch (inst.op) {
		case IR::Opcode::ControlNop: EmitControlNop(state, inst); break;
		case IR::Opcode::Waitcnt: EmitWaitcnt(state, inst); break;
		case IR::Opcode::Barrier: EmitBarrier(state, inst); break;
		case IR::Opcode::Sendmsg: EmitSendmsg(state, inst); break;
		case IR::Opcode::TtraceData:
		case IR::Opcode::InstPrefetch: EmitControlNop(state, inst); break;
		case IR::Opcode::LoadInputF32: EmitLoadInputF32(state, inst); break;
		case IR::Opcode::MoveU32: EmitMoveU32(state, inst); break;
		case IR::Opcode::MoveF32Bits: EmitMoveF32Bits(state, inst); break;
		case IR::Opcode::MoveRelDestU32: EmitMoveRelDestU32(state, inst); break;
		case IR::Opcode::MoveRelSourceU32: EmitMoveRelSourceU32(state, inst); break;
		case IR::Opcode::MoveU64: EmitMoveU64(state, inst); break;
		case IR::Opcode::WqmB64: EmitWqmB64(state, inst); break;
		case IR::Opcode::SaveexecB32: EmitSaveexecB32(state, inst); break;
		case IR::Opcode::SaveexecB64: EmitSaveexecB64(state, inst); break;
		case IR::Opcode::ReadFirstLaneU32: EmitReadFirstLaneU32(state, inst); break;
		case IR::Opcode::ReadLaneU32: EmitReadLaneU32(state, inst); break;
		case IR::Opcode::WriteLaneU32: EmitWriteLaneU32(state, inst); break;
		case IR::Opcode::Permlane16B32: EmitPermlaneB32(state, inst, false); break;
		case IR::Opcode::Permlanex16B32: EmitPermlaneB32(state, inst, true); break;
		case IR::Opcode::AbsI32: EmitAbsI32(state, inst); break;
		case IR::Opcode::IAddU32: EmitBinaryU32(state, inst, OpIAdd); break;
		case IR::Opcode::IAddCarryU32: EmitIAddCarryU32(state, inst); break;
		case IR::Opcode::ISubBorrowU32: EmitISubBorrowU32(state, inst); break;
		case IR::Opcode::ISubBorrowCarryU32: EmitISubBorrowCarryU32(state, inst); break;
		case IR::Opcode::ScalarAddCarryU32: EmitScalarAddCarryU32(state, inst); break;
		case IR::Opcode::ScalarSubBorrowU32: EmitScalarSubBorrowU32(state, inst); break;
		case IR::Opcode::ScalarSubBorrowCarryU32: EmitScalarSubBorrowCarryU32(state, inst); break;
		case IR::Opcode::ScalarSignedAddOverflowI32:
			EmitScalarSignedOverflowI32(state, inst, false);
			break;
		case IR::Opcode::ScalarSignedSubOverflowI32:
			EmitScalarSignedOverflowI32(state, inst, true);
			break;
		case IR::Opcode::ScalarShiftLeftAddCarryU32:
			EmitScalarShiftLeftAddCarryU32(state, inst);
			break;
		case IR::Opcode::ISubU32: EmitBinaryU32(state, inst, OpISub); break;
		case IR::Opcode::IMulU32: EmitBinaryU32(state, inst, OpIMul); break;
		case IR::Opcode::UMulHighU32: EmitMulHighU32(state, inst); break;
		case IR::Opcode::SMulHighI32: EmitMulHighI32(state, inst); break;
		case IR::Opcode::IMadI24U32: EmitMadI32I24(state, inst); break;
		case IR::Opcode::UMadU24U32: EmitMadU32U24(state, inst); break;
		case IR::Opcode::UMadU64U32: EmitUMadU64U32(state, inst); break;
		case IR::Opcode::SadU32: EmitSadU32(state, inst); break;
		case IR::Opcode::IAdd3U32: EmitAdd3U32(state, inst); break;
		case IR::Opcode::IMulI24U32: EmitMulI24U32(state, inst); break;
		case IR::Opcode::UMulU24U32: EmitMulU24U32(state, inst); break;
		case IR::Opcode::IMinI32: EmitMinMaxI32(state, inst, false); break;
		case IR::Opcode::IMaxI32: EmitMinMaxI32(state, inst, true); break;
		case IR::Opcode::IMin3I32: EmitMinMax3U32(state, inst, true, false); break;
		case IR::Opcode::IMax3I32: EmitMinMax3U32(state, inst, true, true); break;
		case IR::Opcode::IMed3I32: EmitMed3U32(state, inst, true); break;
		case IR::Opcode::UMinU32: EmitMinMaxU32(state, inst, false); break;
		case IR::Opcode::UMaxU32: EmitMinMaxU32(state, inst, true); break;
		case IR::Opcode::UMin3U32: EmitMinMax3U32(state, inst, false, false); break;
		case IR::Opcode::UMax3U32: EmitMinMax3U32(state, inst, false, true); break;
		case IR::Opcode::UMed3U32: EmitMed3U32(state, inst, false); break;
		case IR::Opcode::BitwiseAndU32: EmitBinaryU32(state, inst, OpBitwiseAnd); break;
		case IR::Opcode::BitwiseAndU64: EmitBinaryU64(state, inst, OpBitwiseAnd); break;
		case IR::Opcode::BitwiseAndNotU32: EmitBinaryNotRhsU32(state, inst, OpBitwiseAnd); break;
		case IR::Opcode::BitwiseAndNotU64: EmitBinaryNotRhsU64(state, inst, OpBitwiseAnd); break;
		case IR::Opcode::BitwiseOrU32: EmitBinaryU32(state, inst, OpBitwiseOr); break;
		case IR::Opcode::BitwiseOrU64: EmitBinaryU64(state, inst, OpBitwiseOr); break;
		case IR::Opcode::BitwiseOrNotU32: EmitBinaryNotRhsU32(state, inst, OpBitwiseOr); break;
		case IR::Opcode::BitwiseOrNotU64: EmitBinaryNotRhsU64(state, inst, OpBitwiseOr); break;
		case IR::Opcode::BitwiseAndOrU32:
			EmitChainedBinaryU32(state, inst, OpBitwiseAnd, OpBitwiseOr);
			break;
		case IR::Opcode::BitwiseOr3U32:
			EmitChainedBinaryU32(state, inst, OpBitwiseOr, OpBitwiseOr);
			break;
		case IR::Opcode::BitwiseXorU32: EmitBinaryU32(state, inst, OpBitwiseXor); break;
		case IR::Opcode::BitwiseXorU64: EmitBinaryU64(state, inst, OpBitwiseXor); break;
		case IR::Opcode::BitwiseXor3U32:
			EmitChainedBinaryU32(state, inst, OpBitwiseXor, OpBitwiseXor);
			break;
		case IR::Opcode::BitwiseNandU32: EmitBinaryThenNotU32(state, inst, OpBitwiseAnd); break;
		case IR::Opcode::BitwiseNandU64: EmitBinaryThenNotU64(state, inst, OpBitwiseAnd); break;
		case IR::Opcode::BitwiseNorU32: EmitBinaryThenNotU32(state, inst, OpBitwiseOr); break;
		case IR::Opcode::BitwiseNorU64: EmitBinaryThenNotU64(state, inst, OpBitwiseOr); break;
		case IR::Opcode::BitwiseXnorU32: EmitBinaryThenNotU32(state, inst, OpBitwiseXor); break;
		case IR::Opcode::BitwiseXnorU64: EmitBinaryThenNotU64(state, inst, OpBitwiseXor); break;
		case IR::Opcode::BitwiseNotU32: EmitUnaryU32(state, inst, OpNot); break;
		case IR::Opcode::BitwiseNotU64: EmitUnaryU64(state, inst, OpNot); break;
		case IR::Opcode::BitClearU32: EmitBitClearU32(state, inst); break;
		case IR::Opcode::BitSetU32: EmitBitSetU32(state, inst); break;
		case IR::Opcode::BitReverseU32: EmitUnaryU32(state, inst, OpBitReverse); break;
		case IR::Opcode::BitCountU32: EmitBitCountU32(state, inst); break;
		case IR::Opcode::BitCountU64: EmitBitCountU64(state, inst); break;
		case IR::Opcode::BitCountAddU32: EmitBitCountAddU32(state, inst); break;
		case IR::Opcode::MaskedBitCountLowU32: EmitMaskedBitCountU32(state, inst, 0); break;
		case IR::Opcode::MaskedBitCountHighU32: EmitMaskedBitCountU32(state, inst, 1); break;
		case IR::Opcode::BitReplicateB64B32: EmitBitReplicateB64B32(state, inst); break;
		case IR::Opcode::FindLsbU32: EmitFindLsbU32(state, inst); break;
		case IR::Opcode::FindLsbU64: EmitFindLsbU64(state, inst); break;
		case IR::Opcode::FindMsbFromHighU32: EmitFindMsbFromHighU32(state, inst); break;
		case IR::Opcode::FindMsbFromHighU64: EmitFindMsbFromHighU64(state, inst); break;
		case IR::Opcode::BitFieldMaskU32: EmitBitFieldMaskU32(state, inst); break;
		case IR::Opcode::BitFieldMaskU64: EmitBitFieldMaskU64(state, inst); break;
		case IR::Opcode::BitFieldExtractU32: EmitBitFieldExtractU32(state, inst); break;
		case IR::Opcode::BitFieldExtractU64: EmitBitFieldExtractU64(state, inst); break;
		case IR::Opcode::BitFieldExtract3U32: EmitBitFieldExtract3U32(state, inst, false); break;
		case IR::Opcode::BitFieldExtract3I32: EmitBitFieldExtract3U32(state, inst, true); break;
		case IR::Opcode::BitFieldInsertSelectU32: EmitBitFieldInsertSelectU32(state, inst); break;
		case IR::Opcode::BitCompare0B32: EmitBitCompareB32(state, inst, false); break;
		case IR::Opcode::BitCompare1B32: EmitBitCompareB32(state, inst, true); break;
		case IR::Opcode::AlignBitU32: EmitAlignBitU32(state, inst); break;
		case IR::Opcode::ShiftLeftAddU32:
			EmitChainedBinaryU32(state, inst, OpShiftLeftLogical, OpIAdd, true);
			break;
		case IR::Opcode::AddShiftLeftU32:
			EmitChainedBinaryU32(state, inst, OpIAdd, OpShiftLeftLogical, false, true);
			break;
		case IR::Opcode::XorAddU32: EmitChainedBinaryU32(state, inst, OpBitwiseXor, OpIAdd); break;
		case IR::Opcode::ShiftLeftOrU32:
			EmitChainedBinaryU32(state, inst, OpShiftLeftLogical, OpBitwiseOr, true);
			break;
		case IR::Opcode::ShiftLeftLogicalU32: EmitShiftU32(state, inst, OpShiftLeftLogical); break;
		case IR::Opcode::ShiftLeftLogicalU64: EmitShiftLeftLogicalU64(state, inst); break;
		case IR::Opcode::ShiftLeftLogicalU16:
			EmitShiftU16(state, inst, OpShiftLeftLogical, false);
			break;
		case IR::Opcode::ShiftRightLogicalU32:
			EmitShiftU32(state, inst, OpShiftRightLogical);
			break;
		case IR::Opcode::ShiftRightLogicalU64: EmitShiftRightLogicalU64(state, inst); break;
		case IR::Opcode::ShiftRightLogicalU16:
			EmitShiftU16(state, inst, OpShiftRightLogical, false);
			break;
		case IR::Opcode::ShiftRightArithmeticI32:
			EmitShiftU32(state, inst, OpShiftRightArithmetic);
			break;
		case IR::Opcode::ShiftRightArithmeticI16:
			EmitShiftU16(state, inst, OpShiftRightArithmetic, true);
			break;
		case IR::Opcode::SelectU32: EmitSelectU32(state, inst); break;
		case IR::Opcode::SelectMaskU32: EmitSelectMaskU32(state, inst); break;
		case IR::Opcode::SelectF32Bits: EmitSelectF32Bits(state, inst); break;
		case IR::Opcode::SelectMaskF32Bits: EmitSelectMaskF32Bits(state, inst); break;
		case IR::Opcode::SelectU64: EmitSelectU64(state, inst); break;
		case IR::Opcode::PackLowLowU16: EmitPackU16(state, inst, false, false); break;
		case IR::Opcode::PackLowHighU16: EmitPackU16(state, inst, false, true); break;
		case IR::Opcode::PackHighHighU16: EmitPackU16(state, inst, true, true); break;
		case IR::Opcode::PackU16U32: EmitPackU16(state, inst, false, false); break;
		case IR::Opcode::CompareFalse: EmitCompareConstant(state, inst, false); break;
		case IR::Opcode::CompareTrue: EmitCompareConstant(state, inst, true); break;
		case IR::Opcode::CompareEqU32: EmitCompareU32(state, inst, OpIEqual); break;
		case IR::Opcode::CompareNeU32: EmitCompareU32(state, inst, OpINotEqual); break;
		case IR::Opcode::CompareGtU32: EmitCompareU32(state, inst, OpUGreaterThan); break;
		case IR::Opcode::CompareGeU32: EmitCompareU32(state, inst, OpUGreaterThanEqual); break;
		case IR::Opcode::CompareLtU32: EmitCompareU32(state, inst, OpULessThan); break;
		case IR::Opcode::CompareLeU32: EmitCompareU32(state, inst, OpULessThanEqual); break;
		case IR::Opcode::CompareNeU64: EmitCompareNeU64(state, inst); break;
		case IR::Opcode::CompareGtU64: EmitCompareGtU64(state, inst); break;
		case IR::Opcode::CompareMaskEqU32: EmitCompareMaskU32(state, inst, OpIEqual); break;
		case IR::Opcode::CompareMaskNeU32: EmitCompareMaskU32(state, inst, OpINotEqual); break;
		case IR::Opcode::CompareMaskGtU32: EmitCompareMaskU32(state, inst, OpUGreaterThan); break;
		case IR::Opcode::CompareMaskGeU32:
			EmitCompareMaskU32(state, inst, OpUGreaterThanEqual);
			break;
		case IR::Opcode::CompareMaskLtU32: EmitCompareMaskU32(state, inst, OpULessThan); break;
		case IR::Opcode::CompareMaskLeU32: EmitCompareMaskU32(state, inst, OpULessThanEqual); break;
		case IR::Opcode::CompareEqI32: EmitCompareU32(state, inst, OpIEqual); break;
		case IR::Opcode::CompareNeI32: EmitCompareU32(state, inst, OpINotEqual); break;
		case IR::Opcode::CompareGtI32: EmitCompareU32(state, inst, OpSGreaterThan); break;
		case IR::Opcode::CompareGeI32: EmitCompareU32(state, inst, OpSGreaterThanEqual); break;
		case IR::Opcode::CompareLtI32: EmitCompareU32(state, inst, OpSLessThan); break;
		case IR::Opcode::CompareLeI32: EmitCompareU32(state, inst, OpSLessThanEqual); break;
		case IR::Opcode::CompareEqI16: EmitCompareI16(state, inst, OpIEqual); break;
		case IR::Opcode::CompareNeI16: EmitCompareI16(state, inst, OpINotEqual); break;
		case IR::Opcode::CompareGtI16: EmitCompareI16(state, inst, OpSGreaterThan); break;
		case IR::Opcode::CompareGeI16: EmitCompareI16(state, inst, OpSGreaterThanEqual); break;
		case IR::Opcode::CompareLtI16: EmitCompareI16(state, inst, OpSLessThan); break;
		case IR::Opcode::CompareLeI16: EmitCompareI16(state, inst, OpSLessThanEqual); break;
		case IR::Opcode::CompareMaskEqI32: EmitCompareMaskU32(state, inst, OpIEqual); break;
		case IR::Opcode::CompareMaskNeI32: EmitCompareMaskU32(state, inst, OpINotEqual); break;
		case IR::Opcode::CompareMaskGtI32: EmitCompareMaskU32(state, inst, OpSGreaterThan); break;
		case IR::Opcode::CompareMaskGeI32:
			EmitCompareMaskU32(state, inst, OpSGreaterThanEqual);
			break;
		case IR::Opcode::CompareMaskLtI32: EmitCompareMaskU32(state, inst, OpSLessThan); break;
		case IR::Opcode::CompareMaskLeI32: EmitCompareMaskU32(state, inst, OpSLessThanEqual); break;
		case IR::Opcode::CompareEqU16: EmitCompareU16(state, inst, OpIEqual); break;
		case IR::Opcode::CompareNeU16: EmitCompareU16(state, inst, OpINotEqual); break;
		case IR::Opcode::CompareGtU16: EmitCompareU16(state, inst, OpUGreaterThan); break;
		case IR::Opcode::CompareGeU16: EmitCompareU16(state, inst, OpUGreaterThanEqual); break;
		case IR::Opcode::CompareLtU16: EmitCompareU16(state, inst, OpULessThan); break;
		case IR::Opcode::CompareLeU16: EmitCompareU16(state, inst, OpULessThanEqual); break;
		case IR::Opcode::CompareEqF32: EmitCompareF32(state, inst, OpFOrdEqual); break;
		case IR::Opcode::CompareNeF32: EmitCompareF32(state, inst, OpFOrdNotEqual); break;
		case IR::Opcode::CompareGtF32: EmitCompareF32(state, inst, OpFOrdGreaterThan); break;
		case IR::Opcode::CompareGeF32: EmitCompareF32(state, inst, OpFOrdGreaterThanEqual); break;
		case IR::Opcode::CompareLtF32: EmitCompareF32(state, inst, OpFOrdLessThan); break;
		case IR::Opcode::CompareLeF32: EmitCompareF32(state, inst, OpFOrdLessThanEqual); break;
		case IR::Opcode::CompareOrderedF32: EmitCompareOrderedF32(state, inst, true); break;
		case IR::Opcode::CompareUnorderedF32: EmitCompareOrderedF32(state, inst, false); break;
		case IR::Opcode::CompareUnordEqF32: EmitCompareF32(state, inst, OpFUnordEqual); break;
		case IR::Opcode::CompareUnordNeF32: EmitCompareF32(state, inst, OpFUnordNotEqual); break;
		case IR::Opcode::CompareUnordGtF32: EmitCompareF32(state, inst, OpFUnordGreaterThan); break;
		case IR::Opcode::CompareUnordGeF32:
			EmitCompareF32(state, inst, OpFUnordGreaterThanEqual);
			break;
		case IR::Opcode::CompareUnordLtF32: EmitCompareF32(state, inst, OpFUnordLessThan); break;
		case IR::Opcode::CompareUnordLeF32:
			EmitCompareF32(state, inst, OpFUnordLessThanEqual);
			break;
		case IR::Opcode::CompareClassF32: EmitCompareClassF32(state, inst); break;
		case IR::Opcode::CompareEqF16: EmitCompareF16(state, inst, OpFOrdEqual); break;
		case IR::Opcode::CompareNeF16: EmitCompareF16(state, inst, OpFOrdNotEqual); break;
		case IR::Opcode::CompareGtF16: EmitCompareF16(state, inst, OpFOrdGreaterThan); break;
		case IR::Opcode::CompareGeF16: EmitCompareF16(state, inst, OpFOrdGreaterThanEqual); break;
		case IR::Opcode::CompareLtF16: EmitCompareF16(state, inst, OpFOrdLessThan); break;
		case IR::Opcode::CompareLeF16: EmitCompareF16(state, inst, OpFOrdLessThanEqual); break;
		case IR::Opcode::CompareUnordNeF16: EmitCompareF16(state, inst, OpFUnordNotEqual); break;
		case IR::Opcode::CompareMaskEqF16: EmitCompareF16(state, inst, OpFOrdEqual); break;
		case IR::Opcode::CompareMaskNeF16: EmitCompareF16(state, inst, OpFOrdNotEqual); break;
		case IR::Opcode::CompareMaskGtF16: EmitCompareF16(state, inst, OpFOrdGreaterThan); break;
		case IR::Opcode::CompareMaskGeF16:
			EmitCompareF16(state, inst, OpFOrdGreaterThanEqual);
			break;
		case IR::Opcode::CompareMaskLtF16: EmitCompareF16(state, inst, OpFOrdLessThan); break;
		case IR::Opcode::CompareMaskLeF16: EmitCompareF16(state, inst, OpFOrdLessThanEqual); break;
		case IR::Opcode::CompareMaskUnordNeF16:
			EmitCompareF16(state, inst, OpFUnordNotEqual);
			break;
		case IR::Opcode::CompareMaskUnordGeF16:
			EmitCompareF16(state, inst, OpFUnordGreaterThanEqual);
			break;
		case IR::Opcode::CompareMaskEqF32: EmitCompareMaskF32(state, inst, OpFOrdEqual); break;
		case IR::Opcode::CompareMaskNeF32: EmitCompareMaskF32(state, inst, OpFOrdNotEqual); break;
		case IR::Opcode::CompareMaskGtF32:
			EmitCompareMaskF32(state, inst, OpFOrdGreaterThan);
			break;
		case IR::Opcode::CompareMaskGeF32:
			EmitCompareMaskF32(state, inst, OpFOrdGreaterThanEqual);
			break;
		case IR::Opcode::CompareMaskLtF32: EmitCompareMaskF32(state, inst, OpFOrdLessThan); break;
		case IR::Opcode::CompareMaskLeF32:
			EmitCompareMaskF32(state, inst, OpFOrdLessThanEqual);
			break;
		case IR::Opcode::CompareMaskUnordEqF32:
			EmitCompareMaskF32(state, inst, OpFUnordEqual);
			break;
		case IR::Opcode::CompareMaskUnordNeF32:
			EmitCompareMaskF32(state, inst, OpFUnordNotEqual);
			break;
		case IR::Opcode::CompareMaskUnordGtF32:
			EmitCompareMaskF32(state, inst, OpFUnordGreaterThan);
			break;
		case IR::Opcode::CompareMaskUnordGeF32:
			EmitCompareMaskF32(state, inst, OpFUnordGreaterThanEqual);
			break;
		case IR::Opcode::CompareMaskUnordLtF32:
			EmitCompareMaskF32(state, inst, OpFUnordLessThan);
			break;
		case IR::Opcode::CompareMaskUnordLeF32:
			EmitCompareMaskF32(state, inst, OpFUnordLessThanEqual);
			break;
		case IR::Opcode::ConvertU32ToF32: EmitConvertU32ToF32(state, inst); break;
		case IR::Opcode::ConvertI32ToF32: EmitConvertI32ToF32(state, inst); break;
		case IR::Opcode::ConvertF32ToU32: EmitConvertF32ToU32(state, inst); break;
		case IR::Opcode::ConvertF32ToI32: EmitConvertF32ToI32(state, inst); break;
		case IR::Opcode::ConvertF32ToF16: EmitConvertF32ToF16(state, inst); break;
		case IR::Opcode::ConvertF16ToF32: EmitConvertF16ToF32(state, inst); break;
		case IR::Opcode::ConvertU16ToF16: EmitConvertU16ToF16(state, inst); break;
		case IR::Opcode::ConvertF16ToU16: EmitConvertF16ToU16(state, inst); break;
		case IR::Opcode::ConvertI16ToF16: EmitConvertI16ToF16(state, inst); break;
		case IR::Opcode::ConvertF16ToI16: EmitConvertF16ToI16(state, inst); break;
		case IR::Opcode::ConvertRoundPlusInfF32ToI32:
			EmitConvertRoundPlusInfF32ToI32(state, inst);
			break;
		case IR::Opcode::ConvertFloorF32ToI32: EmitConvertFloorF32ToI32(state, inst); break;
		case IR::Opcode::ConvertI4ToOffsetF32: EmitConvertI4ToOffsetF32(state, inst); break;
		case IR::Opcode::LdexpF32: EmitLdexpF32(state, inst); break;
		case IR::Opcode::PackF32ToF16Rtz: EmitPackF32ToF16Rtz(state, inst); break;
		case IR::Opcode::PackSnorm2x16F32:
			EmitPackNormalizedF32(state, inst, GlslPackSnorm2x16);
			break;
		case IR::Opcode::PackUnorm2x16F32:
			EmitPackNormalizedF32(state, inst, GlslPackUnorm2x16);
			break;
		case IR::Opcode::PackU8F32: EmitPackU8F32(state, inst); break;
		case IR::Opcode::PackB32F16: EmitPackB32F16(state, inst); break;
		case IR::Opcode::PackedMadI16:
		case IR::Opcode::PackedMulLoU16:
		case IR::Opcode::PackedAddI16:
		case IR::Opcode::PackedSubI16:
		case IR::Opcode::PackedLshlrevB16:
		case IR::Opcode::PackedLshrrevB16:
		case IR::Opcode::PackedAshrrevI16:
		case IR::Opcode::PackedMaxI16:
		case IR::Opcode::PackedMinI16:
		case IR::Opcode::PackedMadU16:
		case IR::Opcode::PackedAddU16:
		case IR::Opcode::PackedSubU16:
		case IR::Opcode::PackedMaxU16:
		case IR::Opcode::PackedMinU16: EmitPackedInteger16(state, inst); break;
		case IR::Opcode::PackedAddF16:
		case IR::Opcode::PackedMulF16:
		case IR::Opcode::PackedMinF16:
		case IR::Opcode::PackedMaxF16:
		case IR::Opcode::PackedFmaF16: EmitPackedF16(state, inst); break;
		case IR::Opcode::AddF16: EmitBinaryF16(state, inst, OpFAdd); break;
		case IR::Opcode::SubF16: EmitBinaryF16(state, inst, OpFSub); break;
		case IR::Opcode::MulF16: EmitBinaryF16(state, inst, OpFMul); break;
		case IR::Opcode::MinF16: EmitMinMaxF16(state, inst, false); break;
		case IR::Opcode::MaxF16: EmitMinMaxF16(state, inst, true); break;
		case IR::Opcode::FmaF16: EmitFmaF16(state, inst); break;
		case IR::Opcode::Min3F16: EmitMinMax3F16(state, inst, false); break;
		case IR::Opcode::Max3F16: EmitMinMax3F16(state, inst, true); break;
		case IR::Opcode::Med3F16: EmitMed3F16(state, inst); break;
		case IR::Opcode::MadMixF16: EmitMadMixF16(state, inst); break;
		case IR::Opcode::IAddU16: EmitBinaryU16(state, inst, OpIAdd); break;
		case IR::Opcode::ISubI16: EmitBinaryU16(state, inst, OpISub); break;
		case IR::Opcode::IMinI16: EmitMinMaxU16(state, inst, true, false); break;
		case IR::Opcode::IMaxI16: EmitMinMaxU16(state, inst, true, true); break;
		case IR::Opcode::UMinU16: EmitMinMaxU16(state, inst, false, false); break;
		case IR::Opcode::UMaxU16: EmitMinMaxU16(state, inst, false, true); break;
		case IR::Opcode::RcpF16:
		case IR::Opcode::SqrtF16:
		case IR::Opcode::InverseSqrtF16:
		case IR::Opcode::Log2F16:
		case IR::Opcode::Exp2F16:
		case IR::Opcode::FloorF16:
		case IR::Opcode::CeilF16:
		case IR::Opcode::TruncF16:
		case IR::Opcode::RoundEvenF16: EmitSpecialF16(state, inst); break;
		case IR::Opcode::ConvertByteU32ToF32: EmitConvertByteU32ToF32(state, inst); break;
		case IR::Opcode::RcpF32: EmitRcpF32(state, inst); break;
		case IR::Opcode::FractF32: EmitFloatExtInst(state, inst, GlslFract); break;
		case IR::Opcode::TruncF32: EmitFloatExtInst(state, inst, GlslTrunc); break;
		case IR::Opcode::CeilF32: EmitFloatExtInst(state, inst, GlslCeil); break;
		case IR::Opcode::RoundEvenF32: EmitFloatExtInst(state, inst, GlslRoundEven); break;
		case IR::Opcode::FloorF32: EmitFloatExtInst(state, inst, GlslFloor); break;
		case IR::Opcode::Exp2F32: EmitFloatExtInst(state, inst, GlslExp2, false, true); break;
		case IR::Opcode::Log2F32: EmitFloatExtInst(state, inst, GlslLog2, false, true); break;
		case IR::Opcode::InverseSqrtF32:
			EmitFloatExtInst(state, inst, GlslInverseSqrt, false, true);
			break;
		case IR::Opcode::SqrtF32: EmitFloatExtInst(state, inst, GlslSqrt, false, true); break;
		case IR::Opcode::SinF32: EmitFloatExtInst(state, inst, GlslSin, true); break;
		case IR::Opcode::CosF32: EmitFloatExtInst(state, inst, GlslCos, true); break;
		case IR::Opcode::CubeIdF32: EmitCubeF32(state, inst); break;
		case IR::Opcode::CubeScF32: EmitCubeF32(state, inst); break;
		case IR::Opcode::CubeTcF32: EmitCubeF32(state, inst); break;
		case IR::Opcode::CubeMaF32: EmitCubeF32(state, inst); break;
		case IR::Opcode::FAddF32: EmitBinaryF32(state, inst, OpFAdd); break;
		case IR::Opcode::FSubF32: EmitBinaryF32(state, inst, OpFSub); break;
		case IR::Opcode::FMulF32: EmitBinaryF32(state, inst, OpFMul); break;
		case IR::Opcode::FMinF32: EmitMinMaxF32(state, inst, false); break;
		case IR::Opcode::FMaxF32: EmitMinMaxF32(state, inst, true); break;
		case IR::Opcode::FMadF32: EmitMadF32(state, inst); break;
		case IR::Opcode::Dot2AccF32F16: EmitDot2AccF32F16(state, inst); break;
		case IR::Opcode::FMin3F32: EmitMinMax3F32(state, inst, false); break;
		case IR::Opcode::FMax3F32: EmitMinMax3F32(state, inst, true); break;
		case IR::Opcode::FMed3F32: EmitMed3F32(state, inst); break;
		case IR::Opcode::SLoadDword: EmitSLoadDword(state, inst); break;
		case IR::Opcode::SBufferLoadDword:
			EmitMemoryLoadU32(state, inst, IR::ResourceKind::ScalarBuffer, 0, 1);
			break;
		case IR::Opcode::LoadSrtDword: EmitLoadSrtDword(state, inst); break;
		case IR::Opcode::BufferLoadUbyte: EmitBufferLoadUbyte(state, inst); break;
		case IR::Opcode::BufferLoadSbyte: EmitBufferLoadSbyte(state, inst); break;
		case IR::Opcode::BufferLoadUshort: EmitBufferLoadUshort(state, inst); break;
		case IR::Opcode::BufferLoadSshort: EmitBufferLoadSshort(state, inst); break;
		case IR::Opcode::BufferLoadDword: EmitBufferLoadDword(state, inst); break;
		case IR::Opcode::BufferStoreByte:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
				                           AddressSourceCount(inst, 1), 8);
			});
			break;
		case IR::Opcode::BufferStoreShort:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
				                           AddressSourceCount(inst, 1), 16);
			});
			break;
		case IR::Opcode::BufferStoreDword:
			EmitGuardedByExec(state, [&]() { EmitBufferStoreDword(state, inst); });
			break;
		case IR::Opcode::AtomicSwapU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicExchange); });
			break;
		case IR::Opcode::AtomicAddU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicIAdd); });
			break;
		case IR::Opcode::AtomicSubU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicISub); });
			break;
		case IR::Opcode::AtomicSMinI32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicSMin); });
			break;
		case IR::Opcode::AtomicUMinU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicUMin); });
			break;
		case IR::Opcode::AtomicSMaxI32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicSMax); });
			break;
		case IR::Opcode::AtomicUMaxU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicUMax); });
			break;
		case IR::Opcode::AtomicAndU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicAnd); });
			break;
		case IR::Opcode::AtomicOrU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicOr); });
			break;
		case IR::Opcode::AtomicXorU32:
			EmitGuardedByExec(state, [&]() { EmitAtomicU32(state, inst, OpAtomicXor); });
			break;
		case IR::Opcode::FlatLoadUbyte: EmitFlatLoadUbyte(state, inst); break;
		case IR::Opcode::FlatLoadSbyte: EmitFlatLoadSbyte(state, inst); break;
		case IR::Opcode::FlatLoadUshort: EmitFlatLoadUshort(state, inst); break;
		case IR::Opcode::FlatLoadSshort: EmitFlatLoadSshort(state, inst); break;
		case IR::Opcode::FlatLoadDword: EmitFlatLoadDword(state, inst); break;
		case IR::Opcode::FlatStoreByte:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreSubDwordU32(state, inst, inst.memory.kind, 1, inst.src_count - 1u,
				                           8);
			});
			break;
		case IR::Opcode::FlatStoreShort:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreSubDwordU32(state, inst, inst.memory.kind, 1, inst.src_count - 1u,
				                           16);
			});
			break;
		case IR::Opcode::FlatStoreDword:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreU32(state, inst, inst.memory.kind, 1, inst.src_count - 1u);
			});
			break;
		case IR::Opcode::DsReadUbyte:
			EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, 1, 8, false);
			break;
		case IR::Opcode::DsReadSbyte:
			EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, 1, 8, true);
			break;
		case IR::Opcode::DsReadUshort:
			EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, 1, 16, false);
			break;
		case IR::Opcode::DsReadSshort:
			EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, 1, 16, true);
			break;
		case IR::Opcode::DsReadB32: EmitMemoryLoadU32(state, inst, inst.memory.kind, 0, 1); break;
		case IR::Opcode::DsSwizzleB32: EmitDsSwizzleB32(state, inst); break;
		case IR::Opcode::DsConsume: EmitDsAppendConsume(state, inst, OpAtomicISub); break;
		case IR::Opcode::DsAppend: EmitDsAppendConsume(state, inst, OpAtomicIAdd); break;
		case IR::Opcode::DsReadAddtidB32: EmitDsReadAddtidB32(state, inst); break;
		case IR::Opcode::DsMinF32:
			EmitGuardedByExec(state, [&]() { EmitDsFloatMinMaxF32(state, inst, false); });
			break;
		case IR::Opcode::DsMaxF32:
			EmitGuardedByExec(state, [&]() { EmitDsFloatMinMaxF32(state, inst, true); });
			break;
		case IR::Opcode::DsWriteByte:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreSubDwordU32(state, inst, inst.memory.kind, 1, 1, 8);
			});
			break;
		case IR::Opcode::DsWriteShort:
			EmitGuardedByExec(state, [&]() {
				EmitMemoryStoreSubDwordU32(state, inst, inst.memory.kind, 1, 1, 16);
			});
			break;
		case IR::Opcode::DsWriteB32:
			EmitGuardedByExec(state,
			                  [&]() { EmitMemoryStoreU32(state, inst, inst.memory.kind, 1, 1); });
			break;
		case IR::Opcode::DsWriteAddtidB32:
			EmitGuardedByExec(state, [&]() { EmitDsWriteAddtidB32(state, inst); });
			break;
		case IR::Opcode::ImageGetResinfo: EmitImageGetResinfo(state, inst); break;
		case IR::Opcode::ImageGetLod: EmitImageGetLod(state, inst); break;
		case IR::Opcode::ImageLoad: EmitImageLoad(state, inst); break;
		case IR::Opcode::ImageStore:
			EmitGuardedByExec(state, [&]() { EmitImageStore(state, inst); });
			break;
		case IR::Opcode::ImageSample: EmitImageSample(state, inst); break;
		case IR::Opcode::ImageGather4: EmitImageGather4(state, inst); break;
		case IR::Opcode::Export:
			if (ExportUsesPixelValidMask(*state, inst)) {
				EmitUpdatePixelValidMask(state);
			}
			EmitGuardedByExec(state, [&]() { EmitExport(state, inst); });
			break;
		default: break;
	}
}

uint32_t DispatcherTargetValue(EmitterState* state, uint32_t block_id) {
	return BlockLabel(*state, block_id) != 0 ? block_id : UINT32_MAX;
}

void EmitDispatcherStoreTarget(EmitterState* state, uint32_t block_id) {
	state->builder.AddFunction({OpStore, state->dispatch_pc_variable,
	                            ConstantU32(state, DispatcherTargetValue(state, block_id))});
}

void EmitDispatcherExit(EmitterState* state);

bool RegisterForScalarCode(uint32_t code, IR::Register& reg) {
	if (code < 106u) {
		reg = {IR::RegisterFile::Scalar, code};
		return true;
	}
	switch (code) {
		case 106u: reg = {IR::RegisterFile::Vcc, 0}; return true;
		case 107u: reg = {IR::RegisterFile::Vcc, 1}; return true;
		case 124u: reg = {IR::RegisterFile::M0, 0}; return true;
		case 126u: reg = {IR::RegisterFile::Exec, 0}; return true;
		case 127u: reg = {IR::RegisterFile::Exec, 1}; return true;
		default: return false;
	}
}

bool EmitDispatcherLoadScalarCode(EmitterState* state, uint32_t code, uint32_t& value) {
	IR::Register reg;
	if (!RegisterForScalarCode(code, reg)) {
		return false;
	}
	const auto pointer = PointerForRegister(*state, reg);
	if (pointer == 0) {
		return false;
	}
	value = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
	return true;
}

void EmitDispatcherStoreSelectorTarget(EmitterState* state, const CFG::Terminator& term) {
	if (term.indirect_selector_code == UINT32_MAX || term.indirect_selector_values.empty() ||
	    term.indirect_selector_values.size() != term.indirect_selector_targets.size()) {
		EmitDispatcherExit(state);
		return;
	}

	uint32_t selector_value = 0;
	if (!EmitDispatcherLoadScalarCode(state, term.indirect_selector_code, selector_value)) {
		EmitDispatcherExit(state);
		return;
	}

	uint32_t selected = ConstantU32(state, UINT32_MAX);
	for (uint32_t i = 0; i < term.indirect_selector_values.size(); i++) {
		const auto match = state->builder.AllocateId();
		state->builder.AddFunction({OpIEqual, state->bool_type, match, selector_value,
		                            ConstantU32(state, term.indirect_selector_values[i])});
		const auto next = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, next, match,
		     ConstantU32(state, DispatcherTargetValue(state, term.indirect_selector_targets[i])),
		     selected});
		selected = next;
	}
	state->builder.AddFunction({OpStore, state->dispatch_pc_variable, selected});
	state->builder.AddFunction({OpBranch, state->dispatch_after_switch_label});
}

void EmitDispatcherStoreIndirectTarget(EmitterState* state, const CFG::Terminator& term) {
	if (term.indirect_selector_code != UINT32_MAX) {
		EmitDispatcherStoreSelectorTarget(state, term);
		return;
	}

	if (term.indirect_pc_sgpr == UINT32_MAX) {
		EmitDispatcherExit(state);
		return;
	}

	const auto pointer =
	    PointerForRegister(*state, IR::Register {IR::RegisterFile::Scalar, term.indirect_pc_sgpr});
	if (pointer == 0) {
		EmitDispatcherExit(state);
		return;
	}

	const auto pc_value = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->uint_type, pc_value, pointer});
	uint32_t   selected = ConstantU32(state, UINT32_MAX);
	const auto count    = std::min(term.indirect_target_pcs.size(), term.indirect_targets.size());
	for (uint32_t i = 0; i < count; i++) {
		const auto match = state->builder.AllocateId();
		state->builder.AddFunction({OpIEqual, state->bool_type, match, pc_value,
		                            ConstantU32(state, term.indirect_target_pcs[i])});
		const auto next = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, next, match,
		     ConstantU32(state, DispatcherTargetValue(state, term.indirect_targets[i])), selected});
		selected = next;
	}
	state->builder.AddFunction({OpStore, state->dispatch_pc_variable, selected});
	state->builder.AddFunction({OpBranch, state->dispatch_after_switch_label});
}

void EmitDispatcherExit(EmitterState* state) {
	state->builder.AddFunction(
	    {OpStore, state->dispatch_pc_variable, ConstantU32(state, UINT32_MAX)});
	state->builder.AddFunction({OpBranch, state->dispatch_after_switch_label});
}

void EmitDispatcherTerminator(EmitterState* state, const CFG::Terminator& term) {
	switch (term.kind) {
		case CFG::TerminatorKind::Branch:
			if (BlockLabel(*state, term.true_block) == 0) {
				EmitDispatcherExit(state);
				return;
			}
			EmitDispatcherStoreTarget(state, term.true_block);
			state->builder.AddFunction({OpBranch, state->dispatch_after_switch_label});
			return;
		case CFG::TerminatorKind::ConditionalBranch: {
			const auto condition = EmitBranchCondition(state, term.condition);
			const auto selected  = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpSelect, state->uint_type, selected, condition,
			     ConstantU32(state, DispatcherTargetValue(state, term.true_block)),
			     ConstantU32(state, DispatcherTargetValue(state, term.false_block))});
			state->builder.AddFunction({OpStore, state->dispatch_pc_variable, selected});
			state->builder.AddFunction({OpBranch, state->dispatch_after_switch_label});
			return;
		}
		case CFG::TerminatorKind::IndirectBranch:
			EmitDispatcherStoreIndirectTarget(state, term);
			return;
		default: EmitDispatcherExit(state); return;
	}
}

void EmitDispatcherSwitch(EmitterState* state, const IR::Program& program) {
	state->builder.AddFunction({OpLabel, state->dispatch_header_label});
	const auto pc_value = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->uint_type, pc_value, state->dispatch_pc_variable});
	const auto done = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpIEqual, state->bool_type, done, pc_value, ConstantU32(state, UINT32_MAX)});
	state->builder.AddFunction({OpLoopMerge, state->dispatch_merge_label,
	                            state->dispatch_continue_label, LoopControlNone});
	state->builder.AddFunction(
	    {OpBranchConditional, done, state->dispatch_merge_label, state->dispatch_select_label});

	state->builder.AddFunction({OpLabel, state->dispatch_select_label});
	state->builder.AddFunction(
	    {OpSelectionMerge, state->dispatch_after_switch_label, SelectionControlNone});
	std::vector<uint32_t> switch_words = {OpSwitch, pc_value, state->dispatch_default_label};
	for (const auto& block: program.blocks) {
		if (block.id >= state->reachable_blocks.size() || !state->reachable_blocks[block.id]) {
			continue;
		}
		const auto label = BlockLabel(*state, block.id);
		if (label != 0) {
			switch_words.push_back(block.id);
			switch_words.push_back(label);
		}
	}
	state->builder.AddFunction(switch_words);

	state->builder.AddFunction({OpLabel, state->dispatch_default_label});
	EmitDispatcherExit(state);
}

void EmitDispatcherBlocks(EmitterState* state, const IR::Program& program) {
	for (const auto& block: program.blocks) {
		if (block.id >= state->reachable_blocks.size() || !state->reachable_blocks[block.id]) {
			continue;
		}
		state->builder.AddFunction({OpLabel, BlockLabel(*state, block.id)});
		for (const auto& inst: block.instructions) {
			EmitInstruction(state, inst);
		}
		EmitDispatcherTerminator(state, block.terminator);
	}
}

void EmitDispatcherLoopTail(EmitterState* state) {
	state->builder.AddFunction({OpLabel, state->dispatch_after_switch_label});
	state->builder.AddFunction({OpBranch, state->dispatch_continue_label});
	state->builder.AddFunction({OpLabel, state->dispatch_continue_label});
	state->builder.AddFunction({OpBranch, state->dispatch_header_label});
	state->builder.AddFunction({OpLabel, state->dispatch_merge_label});
	EmitReturn(state);
}

void EmitDispatcherFunction(EmitterState* state, const IR::Program& program) {
	state->builder.AddFunction({OpStore, state->dispatch_pc_variable, ConstantU32(state, 0)});
	state->builder.AddFunction({OpBranch, state->dispatch_header_label});
	EmitDispatcherSwitch(state, program);
	EmitDispatcherBlocks(state, program);
	EmitDispatcherLoopTail(state);
}

void EmitFunction(EmitterState* state, const IR::Program& program) {
	state->builder.AddFunction(
	    {OpFunction, state->void_type, state->main_func, FunctionControlNone, state->func_type});
	state->builder.AddFunction({OpLabel, state->entry_label});

	EmitRegisterVariables(state);
	EmitComputeInputRegisters(state);
	EmitPixelInputRegisters(state);
	EmitVertexInputRegisters(state);

	// RADAR SENTINEL BASLANGICI (`PSEMU_MESH_RING_RADAR=1`).
	//
	// Tarama dizisi Workgroup bellegi ve HICBIR baslangic degeri yok; Vulkan'da
	// bu bellek tanimsiz baslar. "0 = yazilmadi" varsayimi bu yuzden sansa
	// dayaniyordu - min/max izlemede dogrudan YANLIS sonuc uretirdi (cop bir
	// baslangic min'i ezer). Sentinel'leri acikca yaziyoruz:
	//   scan[69] = 0           GS okuma indeksi (yazilinca dolar)
	//   scan[70] = 0xFFFFFFFF  ES yazma MIN
	//   scan[71] = 0           ES yazma MAX
	// Yalnizca lane 0 dokunuyor - min/max guncellemelerini de lane 0 yaptigi
	// icin ayni invocation icinde sirali, bariyer gerekmiyor.
	if (state->stage == ShaderType::Mesh && MeshRingRadarEnabled() &&
	    state->mesh_scan_variable != 0) {
		const auto lane = EmitLocalInvocationIndex(state);
		// 82/83 yuvalarini HER lane kendi yuvasi icin sifirlar - secili lane
		// sifirlarsa digerinin yazimini silebilirdi.
		{
			const auto iki = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpULessThan, state->bool_type, iki, lane, ConstantU32(state, 2u)});
			EmitIfCondition(state, iki, [&]() {
				const auto yuva = state->builder.AllocateId();
				const auto p82  = state->builder.AllocateId();
				state->builder.AddFunction(
				    {OpIAdd, state->uint_type, yuva, lane, ConstantU32(state, 82u)});
				state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, p82,
				                            state->mesh_scan_variable, yuva});
				state->builder.AddFunction({OpStore, p82, ConstantU32(state, 0)});
			});
		}
		const auto is0  = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpIEqual, state->bool_type, is0, lane, ConstantU32(state, MeshRadarLane())});
		EmitIfCondition(state, is0, [&]() {
			const uint32_t slot[9] = {69u, 70u, 71u, 72u, 73u, 74u, 75u, 76u, 77u};
			const uint32_t init[9] = {0u, 0xFFFFFFFFu, 0u, 0u, 0u,
			                          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
			for (uint32_t i = 0; i < 9; i++) {
				const auto p = state->builder.AllocateId();
				state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, p,
				                            state->mesh_scan_variable,
				                            ConstantU32(state, slot[i])});
				state->builder.AddFunction({OpStore, p, ConstantU32(state, init[i])});
			}
		});
	}

	// DENEY (PSEMU_MESH_ALLOC=erken): mesh tahsisini GIRIS BLOGUNA, sabit
	// donanim limitleriyle al.
	//
	// Neden: olculen NGG GS'te GS_ALLOC_REQ kosullu ("s_and_b32 vcc_lo, s3,
	// 0x0f000000" + "s_cbranch_scc1") ve yalnizca BIR dalga gonderiyor - yani
	// is grubu genelinde TEKDUZE DEGIL. spirv-val ve glslang bunu statik olarak
	// kabul etti, ama Vulkan mesh tahsisinin tekduze olmasini bekliyor olabilir.
	// Giris blogu kosulsuz ve tekduze; burada tahsis edip sendmsg'i no-op'a
	// cevirirsek, device lost'un sebebi bu muydu sorusu KESIN cevaplanir.
	if (state->stage == ShaderType::Mesh && MeshEagerAllocEnabled()) {
		state->builder.AddFunction({OpSetMeshOutputsEXT,
		                            ConstantU32(state, MeshOutputVertices(*state)),
		                            ConstantU32(state, MeshOutputPrimitives(*state))});
	}

	// MOD 3 (PSEMU_MESH_FIXPOS=3): "hello ucgeni oyunun govdesinin ICINDE".
	//
	// Mod 2 sabit sayilar + sabit indekslerle de siyah kaldi. Geriye tek
	// aciklama kaliyor: disavurum blogu calisma aninda TEKDUZE DEGIL - NGG'de
	// GS_ALLOC_REQ ve POS0 kosullu, tek dalga/tek lane yurutuyor. Vulkan ise
	// OpSetMeshOutputsEXT'i is grubu genelinde tekduze bekliyor; ustelik yalniz
	// lane 0 yaziyorsa vertex 1-2 hic yazilmaz ve ucgen dejenere olur.
	//
	// Burasi GIRIS BLOGU: kosulsuz ve tekduze. Tahsis + uc kose + indeksler
	// hepsi burada yazilir; oyunun kendi disavurumlari da (mod >= 1 oldugu icin)
	// ayni degerleri yazmaya devam eder, zarari yok.
	//
	//   ucgen cikarsa -> sorun TEKDUZELIK; NGG kosullu tahsisini is grubu
	//                    genelinde yeniden yapilandirmamiz gerekiyor
	//   siyah kalirsa -> sorun shader'da DEGIL; boru tesisati recompile edilen
	//                    programda hello'dakinden farkli davraniyor
	if (state->stage == ShaderType::Mesh && (MeshProbeBlock() != -1 || MeshProbeLo() >= 0)) {
		// PROBE: tahsis GIRIS BLOGUNDA (kosulsuz, tekduze). Yazimlar hedef
		// blok(lar)da. Tahsis calisir ama yazim olmazsa ucgen dejenere olur
		// ve ekran siyah kalir -> "o blok CALISMIYOR" demektir.
		state->builder.AddFunction(
		    {OpSetMeshOutputsEXT, ConstantU32(state, 3u), ConstantU32(state, 1u)});
	}
	if (state->stage == ShaderType::Mesh && MeshFixedPositionMode() >= 3) {
		state->builder.AddFunction(
		    {OpSetMeshOutputsEXT, ConstantU32(state, 3u), ConstantU32(state, 1u)});

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
		printf("[MS-GIRIS] mod %d: giris blogunda kosulsuz tahsis(3,1) + 3 kose + indeks(0,1,2) "
		       "yazildi (per_vertex=%u prim_idx=%u)\n",
		       MeshFixedPositionMode(), state->per_vertex_variable,
		       state->mesh_prim_indices_variable);
		fflush(stdout);

		// MOD 4: oyunun GOVDESINE hic girme, hemen don.
		//
		// Mod 3'te veri HELLO ile birebir ayni ve kosulsuz yazildigi halde
		// siyah kaldi. Geriye iki degisken kaldi: (a) oyunun shader govdesi,
		// (b) programin kendi bildirimleri (LocalSize=64 vs HELLO'nun 1,
		// cikti limitleri, arayuz). Mod 4 govdeyi tamamen cikarir; bildirimler
		// ve boru tesisati aynen kalir.
		//
		//   ucgen cikarsa -> suclu GOVDE (takiliyor, donmuyor ya da durumu
		//                    bozuyor)
		//   siyah kalirsa -> suclu BILDIRIMLER/BORU TESISATI; HELLO ile
		//                    recompile edilen programin farki orada
		if (MeshFixedPositionMode() >= 4) {
			printf("[MS-GIRIS] mod 4: govde ATLANDI, dogrudan OpReturn\n");
			fflush(stdout);
			EmitReturn(state);
			state->builder.AddFunction({OpFunctionEnd});
			return;
		}
	}
	if (state->dispatcher_fallback) {
		EmitDispatcherFunction(state, program);
		state->builder.AddFunction({OpFunctionEnd});
		return;
	}

	const auto entry_block_label = BlockLabel(*state, 0);
	if (entry_block_label != 0) {
		state->builder.AddFunction({OpBranch, entry_block_label});
	} else {
		EmitReturn(state);
	}

	// KENAR HARITASI (PSEMU_CFG_HARITA=1).
	//
	// spirv-val hatalari SPIR-V kimlikleriyle konusuyor ("block '265' exits
	// the selection headed by '696'"), bizim tum sondalarimiz ise IC BLOK
	// numaralariyla. Ikisi arasinda koprü yoktu. Burasi tam olarak SPIR-V
	// etiketinin bilindigi yer; her blok icin etiket <-> ic kimlik esleme
	// tablosunu, ardillari ve merge/continue bloklarini birlikte doker.
	// Boylece spirv-val'in isaret ettigi kimlik dogrudan aranabilir hale gelir.
	const bool dump_map = state->stage == ShaderType::Mesh && CfgMapDumpEnabled();
	if (dump_map) {
		printf("[CFG-KENAR] === asama=Mesh blok_sayisi=%zu ===\n", program.blocks.size());
	}
	uint32_t emitted_block_index = 0;
	for (const auto& block: program.blocks) {
		if (block.id >= state->reachable_blocks.size() || !state->reachable_blocks[block.id]) {
			continue;
		}
		if (dump_map) {
			const auto& t     = block.terminator;
			const auto  self  = BlockLabel(*state, block.id);
			const auto  tl    = t.true_block != UINT32_MAX ? BlockLabel(*state, t.true_block) : 0;
			const auto  fl    = t.false_block != UINT32_MAX ? BlockLabel(*state, t.false_block) : 0;
			const auto  ml    = t.merge_block != UINT32_MAX ? BlockLabel(*state, t.merge_block) : 0;
			const auto  cl    = t.continue_block != UINT32_MAX ? BlockLabel(*state, t.continue_block) : 0;
			printf("[CFG-KENAR] %%%u (ic=%u) tur=%d dongu_basligi=%d | dogru=%%%u(ic=%u) "
			       "yanlis=%%%u(ic=%u) | merge=%%%u(ic=%u) continue=%%%u(ic=%u)\n",
			       self, block.id, static_cast<int>(t.kind), t.loop_header ? 1 : 0, tl,
			       t.true_block, fl, t.false_block, ml, t.merge_block, cl, t.continue_block);
			fflush(stdout);
		}
		state->builder.AddFunction({OpLabel, BlockLabel(*state, block.id)});
		// IKILI ARAMA (`PSEMU_MESH_PROBE_BLOCK=<n>`): kosulsuz hello ucgenini
		// N'inci blogun BASINA koy ve ekrana bak.
		//   KIRMIZI -> yurutme buraya ULASIYOR, kesinti DAHA ASAGIDA
		//   SIYAH   -> buraya bile ulasilmiyor, kesinti DAHA YUKARIDA
		// Elimizdeki iki referans: giris blogu KIRMIZI, tahsis noktasi SIYAH.
		// Aradaki ilk siyah blok, yurutmeyi kesen kapidir.
		const bool in_probe_range =
		    MeshProbeLo() >= 0 && MeshProbeHi() >= 0 &&
		    static_cast<int>(emitted_block_index) >= MeshProbeLo() &&
		    static_cast<int>(emitted_block_index) <= MeshProbeHi();
		if (state->stage == ShaderType::Mesh &&
		    (MeshProbeBlock() == -2 || in_probe_range ||
		     (MeshProbeBlock() >= 0 &&
		      emitted_block_index == static_cast<uint32_t>(MeshProbeBlock())))) {
			EmitMeshProbeTriangle(state, block.id, emitted_block_index, false);
		}
		emitted_block_index++;
		for (const auto& inst: block.instructions) {
			EmitInstruction(state, inst);
		}
		EmitTerminator(state, block.terminator);
	}
	if (state->stage == ShaderType::Mesh && MeshProbeBlock() >= 0) {
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 32) {
			std::printf("[MS-PROBE] shader toplam blok=%u (istenen indeks=%d)\n",
			            emitted_block_index, MeshProbeBlock());
			std::fflush(stdout);
		}
	}

	state->builder.AddFunction({OpFunctionEnd});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
