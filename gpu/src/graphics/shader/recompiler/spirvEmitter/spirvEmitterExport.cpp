#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

#include "graphics/host_gpu/psemuGfxTrace.h"

#include <atomic>
#include <cstdio>

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
	// KESKIN NISANCI: her renk hedefi tam olarak (0,0,0,1) donuyor - asagidaki
	// `components` varsayilanlarinin BIREBIR kendisi. Yani hicbir bilesen
	// yazilmiyor olabilir. Etkin-maske testi `en >> pair_index` (bit 0,1)
	// kullaniyor; sikistirilmis disavurumda donanim cift secimini bit 0 ve 2
	// ile yapar (bu dosyanin 74. satiri zaten `en & 0x5` biliyor). Karar
	// vermeden ONCE gercek `en`/`compr` degerlerini olc.
	if (PsemuGfxTrace()) {
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 200) {
			std::printf("[DISAVURUM] tur=%u compr=%d en=0x%01x src_sayisi=%u hedef=%u\n",
			            static_cast<uint32_t>(inst.export_info.kind),
			            static_cast<int>(inst.export_info.compr),
			            static_cast<uint32_t>(inst.export_info.en), inst.src_count,
			            static_cast<uint32_t>(inst.export_info.target));
			std::fflush(stdout);
		}
	}
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

// NGG VERTEX COMPACTION.
//
// AMD donaniminda `exp pos0` bir yuva indeksi ALMAZ: donanim o anki exec
// maskesinden hayatta kalan lane'leri otomatik SIKISTIRIR ve 0,1,2...
// yuvalarina sirayla dizer. Lane 35/42/61 hayattaysa yuvalari 0/1/2 olur.
//
// Emulator ise `outPerVertex[gl_LocalInvocationIndex]` ile 1:1 esleme
// yapiyordu; lane 35 dogrudan 35. yuvaya yazmaya calisiyor, tahsis edilen
// yuva sayisi asildigi icin yazim cope gidiyor ve ilkel dejenere oluyor.
// OLCULDU (6099300): deger ayni fonksiyondan gelirken yuva `lane` ise
// SIYAH, `lane%3` ise KIRMIZI - yani ulasan lane indeksleri 3'ten buyuk.
//
// Dogru karsilik: aktif lane'ler arasinda KACINCI oldugumuz. SPIR-V'de
// bunun tam esdegeri ballot + EXCLUSIVE SCAN bit sayimi.
//
// NOT: emulatorun `exec`i 64-bit lane maskesi DEGIL, invocation basina
// 0/1 boolean. Bu yuzden `popcount(exec & below)` DOGRU SONUC VERMEZ;
// once ballot ile gercek capraz-lane resmi toplanmali. EmitExecActiveBool
// tam olarak "bu cagri aktif mi" yuklemini veriyor.
uint32_t EmitNggCompactedIndex(EmitterState* state) {
	// YUKLEM SECIMI KRITIK. Ilk denemede `EmitExecActiveBool` kullandim -
	// YANLIS: o, emule edilen `exec` yazmacini okuyor ve bu yazmac
	// invocation basina 0/1'e cokmus durumda (bkz. `exec_lo = select(c,1,0)`).
	// Tum lane'lerde 1 ise ballot 64 bit dolu cikar, exclusive scan yine
	// 0..63 verir - yani lane indeksinin AYNISI. Olculdu: COMPACT+FIXPOS1
	// SIYAH kaldi.
	//
	// Dogru yuklem SPIR-V'nin KENDI aktif-invocation kumesi: sabit `true`
	// ile alinan ballot, o komutu O AN yuruten cagrilari verir - donanimdaki
	// `exec`in disavurum anindaki karsiligi budur.
	const auto active = state->builder.AllocateId();
	state->builder.AddFunction({OpINotEqual, state->bool_type, active, ConstantU32(state, 1),
	                            ConstantU32(state, 0)});
	const auto ballot = state->builder.AllocateId();
	const auto index  = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
	                            ConstantU32(state, ScopeSubgroup), active});
	state->builder.AddFunction({OpGroupNonUniformBallotBitCount, state->uint_type, index,
	                            ConstantU32(state, ScopeSubgroup), GroupOperationExclusiveScan,
	                            ballot});
	return index;
}

bool MeshCompactionEnabled() {
	static const bool on = [] {
		const char* env = std::getenv("PSEMU_MESH_COMPACT");
		return env != nullptr && env[0] == '1';
	}();
	return on;
}

// PSEMU_MESH_FIXPOS=1 deneyi: lane indeksine gore garantili hello-triangle
// kosesi uretir. Kosler HELLO deneyinde EKRANA DUSTUGU DOGRULANAN degerler
// (bkz. meshHelloSpirv.h / hello.mesh):
//     idx%3 == 0 -> (-0.8, -0.8, 0, 1)
//     idx%3 == 1 -> ( 0.8, -0.8, 0, 1)
//     idx%3 == 2 -> ( 0.0,  0.8, 0, 1)
// Dallanmasiz: ic ice OpSelect. Skaler kosullu OpSelect'in vektor
// operandlarla kullanimi SPIR-V 1.4'te gecerli - Mesh zaten 1.4 uretiyor
// (Asama 1'deki kosullu versiyonlama).
static uint32_t EmitFixedTrianglePosition(EmitterState* state, uint32_t lane) {
	const auto make = [&](float x, float y) {
		const auto v = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, v,
		                            ConstantF32Value(state, x), ConstantF32Value(state, y),
		                            ConstantF32Value(state, 0.0f), ConstantF32Value(state, 1.0f)});
		return v;
	};
	const auto c0 = make(-0.8f, -0.8f);
	const auto c1 = make(0.8f, -0.8f);
	const auto c2 = make(0.0f, 0.8f);

	const auto m = state->builder.AllocateId();
	state->builder.AddFunction({OpUMod, state->uint_type, m, lane, ConstantU32(state, 3)});
	const auto eq0 = state->builder.AllocateId();
	const auto eq1 = state->builder.AllocateId();
	state->builder.AddFunction({OpIEqual, state->bool_type, eq0, m, ConstantU32(state, 0)});
	state->builder.AddFunction({OpIEqual, state->bool_type, eq1, m, ConstantU32(state, 1)});
	const auto pick1 = state->builder.AllocateId();
	const auto pick0 = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->vec4_float_type, pick1, eq1, c1, c2});
	state->builder.AddFunction({OpSelect, state->vec4_float_type, pick0, eq0, c0, pick1});
	return pick0;
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
	// MOD 3: indeksler giris blogunda yazildi; burasi tamamen susar.
	if (MeshFixedPositionMode() >= 3) {
		return;
	}

	// MOD 2: paketli indeksi acmak yerine SABIT (0,1,2) yaz. Deneyin amaci
	// 3x10-bit acma isini supheli listesinden cikarmak. Yazma yeri de sabit
	// primitif 0 - oyunun lane indeksi de degiskenlerden biri olmasin.
	if (MeshFixedPositionMode() >= 2) {
		const auto vector  = state->builder.AllocateId();
		const auto pointer = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vector,
		                            ConstantU32(state, 0), ConstantU32(state, 1),
		                            ConstantU32(state, 2)});
		state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, pointer,
		                            state->mesh_prim_indices_variable, ConstantU32(state, 0)});
		state->builder.AddFunction({OpStore, pointer, vector});
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

	// KIRPMA DENEYI: "cikis" modunda mesh disavurumlarinin tamami atlanir.
	if (mesh && (MeshNoopMode() & 4) != 0) {
		return;
	}

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

	auto value = ApplyMrtExportMapping(state, inst, EmitExportVec4F32(state, inst));

	// BISECTION - `PSEMU_PS_FIXCOLOR=1`: PS'in MRT0'a yazdigi RGB'yi ez.
	//
	// Olculdu: rasterizasyon, renk maskesi (0xf), harmanlama (kapali),
	// hedef baglama ve dogrulama TEMIZ; cizimler gorunur alanin TAMAMINA
	// alfa=1.0 yaziyor ama RGB=0. Yani siyahi ureten, PS'in KENDI ciktisi.
	// Bu deney o hipotezi ikiye boler:
	//   ekran KIRMIZI olursa -> rasterizer/OM/sunum zinciri KUSURSUZ,
	//                            sorun yalnizca shader girdileri/govdesinde
	//   ekran SIYAH kalirsa  -> disavurum ile ekran arasinda HALA
	//                            gorulmemis bir katman var
	//
	// KAPSAM KANITI SART: deneyin uygulandigini gostermeden "siyah kaldi"
	// sonucu ANLAMSIZDIR - bu oturumda sifir komut ezip "kanit" gibi
	// gorunen bir deney zaten bir kez yasandi (FIXPOS mod 1-4, shader
	// gecersizken kosmustu ve hicbiri GPU'ya ulasmamisti).
	if (inst.export_info.kind == IR::ExportTargetKind::Mrt) {
		static const bool fix_color = [] {
			const char* env = std::getenv("PSEMU_PS_FIXCOLOR");
			return env != nullptr && env[0] == '1';
		}();
		if (fix_color) {
			const auto red = state->builder.AllocateId();
			state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, red,
			                            ConstantF32(state, 0x3f800000u), ConstantF32(state, 0u),
			                            ConstantF32(state, 0u),
			                            ConstantF32(state, 0x3f800000u)});
			value = red;
			if (PsemuGfxTrace()) {
				static std::atomic<uint32_t> s_n {0};
				if (s_n.fetch_add(1) < 200) {
					std::printf("[PS-SABITRENK] MRT disavurumu EZILDI -> (1,0,0,1) hedef=%u "
					            "compr=%d en=0x%01x\n",
					            static_cast<uint32_t>(inst.export_info.target),
					            static_cast<int>(inst.export_info.compr),
					            static_cast<uint32_t>(inst.export_info.en));
					std::fflush(stdout);
				}
			}
		}
	}

	// Mesh'te her cikti bir DIZI: cagri kendi lane indeksine yazar. Olculen NGG
	// GS'te bu indeks v13 = (dalga_id << 6) + mbcnt, yani gl_LocalInvocationIndex.
	// Sinir denetimi (v13 < vertex_sayisi) shader'in KENDISINDE exec maskesi
	// olarak zaten var; burada tekrar uretmiyoruz.
	if (inst.export_info.kind == IR::ExportTargetKind::Position) {
		// TAM KAPSAM DISAVURUMU (`PSEMU_MESH_OMNILANE=1`).
		//
		// Olculdu: bloklar calisiyor, tahsis calisiyor, vertex sayisi > 0,
		// pozisyon degerleri dogru - ama FIXPOS 1/2 SIYAH, FIXPOS 3 KIRMIZI.
		// Uc modun degerleri ayni; tek fark mod 3'un UC YUVAYA DA acikca
		// yazmasi. Yani suphe: disavuruma yalnizca TEK lane ulasiyor (ya da
		// hepsi ayni lane indeksini uretiyor) ve tek vertex yazilip ilkel
		// dejenere oluyor.
		//
		// Bu kip lane indeksini TAMAMEN devre disi birakir: buraya ulasan
		// HER cagri ucgenin UC kosesini birden yazar. Tahsisi ve oyunun
		// disavurumlarini DEGISTIRMEZ (mod 3'ten farki bu) - yani soru
		// yalnizca "bu noktaya birisi ulasiyor mu".
		//   KIRMIZI -> en az bir cagri ULASIYOR; kusur lane indeksi/exec
		//              maskesi emulasyonunda MUHURLENIR
		//   SIYAH   -> hicbir cagri saglam varamiyor; bu "sayi > 0"
		//              telemetrisiyle CELISIR ve baska bir yola cikar
		static const bool omni_lane = [] {
			const char* env = std::getenv("PSEMU_MESH_OMNILANE");
			return env != nullptr && env[0] == '1';
		}();
		if (mesh && omni_lane) {
			const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
			for (uint32_t slot = 0; slot < 3; slot++) {
				const auto corner = state->builder.AllocateId();
				const auto slot_ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, corner,
				                            ConstantF32Value(state, corners[slot][0]),
				                            ConstantF32Value(state, corners[slot][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, slot_ptr,
				                            variable, ConstantU32(state, slot),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, slot_ptr, corner});
			}
			if (state->mesh_prim_indices_variable != 0) {
				const auto tri = state->builder.AllocateId();
				const auto tri_ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, tri,
				                            ConstantU32(state, 0), ConstantU32(state, 1),
				                            ConstantU32(state, 2)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, tri_ptr,
				                            state->mesh_prim_indices_variable,
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, tri_ptr, tri});
			}
			static std::atomic<uint32_t> s_o {0};
			if (s_o.fetch_add(1) < 32) {
				std::printf("[MS-OMNILANE] POS0 noktasinda 3 kose + indeks LANE'DEN BAGIMSIZ "
				            "yazildi (pc=0x%08x)\n",
				            inst.pc);
				std::fflush(stdout);
			}
			return;
		}
		// LANE KIMLIK TELEMETRISI (`PSEMU_MESH_LANE_PROBE=1`).
		//
		// OMNILANE kirmizi verdi: disavuruma EN AZ BIR cagri ulasiyor ve
		// oyunun kendi tahsisi ucgene yetiyor. Kalan soru: oraya KAC FARKLI
		// lane indeksi ulasiyor?
		//
		// Bu kip HELLO kosesini `lane % 3`e gore secer VE ayni indeksi yuva
		// olarak kullanir (`outPerVertex[lane % 3]`). Oyunun kendi pozisyon
		// matematigi denkleme GIRMEZ.
		//   KIRMIZI -> en az 3 FARKLI lane indeksi geliyor; mbcnt ve exec
		//              maskesi calisiyor, suc oyunun veri okumasinda
		//   SIYAH   -> ya tek lane geciyor (exec kapisi digerlerini oldurdu)
		//              ya da hepsi ayni indeksi uretiyor (mbcnt boolean
		//              cokusu). Iki halde de tek yuva yazilir.
		// AKTIF LANE SAYIMI (`PSEMU_MESH_COUNT_PROBE=1`).
		//
		// `lane%3` KIRMIZI, compaction (0..N-1) SIYAH. Compaction aktif
		// lane'leri yayar; `%3` ise N ne olursa olsun yazimlari 0,1,2'ye
		// KATLAR. Yani `%3`un calismasi, N'in tahsis edilen vertex
		// sayisindan BUYUK oldugunun dolayli gostergesi - compaction o
		// yuzden aralik disina (OOB) tasip discard ediliyor.
		//
		// Bu kip dolayli gostergeyi DOGRUDAN olcuyor: disavurum aninda
		// hayatta olan toplam cagri sayisini ballot+Reduce ile al, oyunun
		// KENDI vertex sayisiyla (m0 & 0xfff) karsilastir ve yalnizca
		// `aktif > vertex_sayisi` iken ucgeni yaz.
		//   KIRMIZI -> exec maskesi lane'leri FILTRELEYEMIYOR; gereginden
		//              fazla cagri disavuruma akiyor (OOB'un kaynagi)
		//   SIYAH   -> aktif sayi sinir icinde; OOB baska bir sebepten
		static const bool count_probe = [] {
			const char* env = std::getenv("PSEMU_MESH_COUNT_PROBE");
			return env != nullptr && env[0] == '1';
		}();
		if (mesh && count_probe) {
			const auto true_id = state->builder.AllocateId();
			const auto ballot  = state->builder.AllocateId();
			const auto active  = state->builder.AllocateId();
			state->builder.AddFunction({OpINotEqual, state->bool_type, true_id,
			                            ConstantU32(state, 1), ConstantU32(state, 0)});
			state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
			                            ConstantU32(state, ScopeSubgroup), true_id});
			state->builder.AddFunction({OpGroupNonUniformBallotBitCount, state->uint_type, active,
			                            ConstantU32(state, ScopeSubgroup), GroupOperationReduce,
			                            ballot});
			const auto m0_ptr = PointerForRegister(*state, {IR::RegisterFile::M0, 0});
			uint32_t   limit  = ConstantU32(state, 3u);
			if (m0_ptr != 0) {
				const auto m0_value = state->builder.AllocateId();
				const auto verts    = state->builder.AllocateId();
				state->builder.AddFunction({OpLoad, state->uint_type, m0_value, m0_ptr});
				state->builder.AddFunction({OpBitwiseAnd, state->uint_type, verts, m0_value,
				                            ConstantU32(state, 0xfffu)});
				limit = verts;
			}
			const auto too_many = state->builder.AllocateId();
			const auto write_b  = state->builder.AllocateId();
			const auto merge_b  = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpUGreaterThan, state->bool_type, too_many, active, limit});
			state->builder.AddFunction({OpSelectionMerge, merge_b, 0u});
			state->builder.AddFunction({OpBranchConditional, too_many, write_b, merge_b});
			state->builder.AddFunction({OpLabel, write_b});
			const auto lane_id = EmitLocalInvocationIndex(state);
			const auto slot    = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpUMod, state->uint_type, slot, lane_id, ConstantU32(state, 3u)});
			const auto value   = EmitFixedTrianglePosition(state, lane_id);
			const auto slot_ptr = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, slot_ptr,
			                            variable, slot, ConstantU32(state, 0)});
			state->builder.AddFunction({OpStore, slot_ptr, value});
			if (state->mesh_prim_indices_variable != 0) {
				const auto tri     = state->builder.AllocateId();
				const auto tri_ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, tri,
				                            ConstantU32(state, 0), ConstantU32(state, 1),
				                            ConstantU32(state, 2)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, tri_ptr,
				                            state->mesh_prim_indices_variable,
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, tri_ptr, tri});
			}
			state->builder.AddFunction({OpBranch, merge_b});
			state->builder.AddFunction({OpLabel, merge_b});
			static std::atomic<uint32_t> s_c {0};
			if (s_c.fetch_add(1) < 32) {
				std::printf("[MS-COUNT-PROBE] aktif_lane > m0_vertex_sayisi ise ucgen (pc=0x%08x)\n",
				            inst.pc);
				std::fflush(stdout);
			}
			return;
		}
		static const bool lane_probe = [] {
			const char* env = std::getenv("PSEMU_MESH_LANE_PROBE");
			return env != nullptr && env[0] == '1';
		}();
		if (mesh && lane_probe) {
			const auto lane   = EmitLocalInvocationIndex(state);
			const auto corner = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpUMod, state->uint_type, corner, lane, ConstantU32(state, 3u)});
			const auto value    = EmitFixedTrianglePosition(state, lane);
			const auto slot_ptr = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, slot_ptr,
			                            variable, corner, ConstantU32(state, 0)});
			state->builder.AddFunction({OpStore, slot_ptr, value});
			if (state->mesh_prim_indices_variable != 0) {
				const auto tri     = state->builder.AllocateId();
				const auto tri_ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, tri,
				                            ConstantU32(state, 0), ConstantU32(state, 1),
				                            ConstantU32(state, 2)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, tri_ptr,
				                            state->mesh_prim_indices_variable,
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, tri_ptr, tri});
			}
			static std::atomic<uint32_t> s_l {0};
			if (s_l.fetch_add(1) < 32) {
				std::printf("[MS-LANE-PROBE] POS0: kose ve yuva = lane%%3 (pc=0x%08x)\n", inst.pc);
				std::fflush(stdout);
			}
			return;
		}
		const auto pointer = state->builder.AllocateId();
		if (mesh) {
			const auto lane = MeshCompactionEnabled() ? EmitNggCompactedIndex(state)
			                                          : EmitLocalInvocationIndex(state);
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer,
			                            variable, lane, ConstantU32(state, 0)});
			// KAPSAM KANITI: deneyin gercekten uygulandigini gostermeden
			// "siyah kaldi" sonucu ANLAMSIZDIR - sifir komut ezip "kanit"
			// gibi gorunen bir deney bu oturumda zaten bir kez yasandi.
			const bool fix = MeshFixedPositionEnabled();
			{
				static std::atomic<uint32_t> s_pos {0};
				const auto n = s_pos.fetch_add(1) + 1;
				if (n <= 8) {
					printf("[MS-POS] mesh POS0 disavurumu #%u (pc=0x%08x) fixpos=%d\n", n, inst.pc,
					       fix ? 1 : 0);
					fflush(stdout);
				}
			}
			// MOD 3: pozisyonlar giris blogunda yazildi; oyunun yazmasi susar.
			if (MeshFixedPositionMode() < 3) {
				const auto stored = fix ? EmitFixedTrianglePosition(state, lane) : value;
				state->builder.AddFunction({OpStore, pointer, stored});
			}
		} else {
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, pointer,
			                            variable, ConstantU32(state, 0)});
			state->builder.AddFunction({OpStore, pointer, value});
		}
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
