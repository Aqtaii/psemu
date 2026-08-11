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

// LDS PREFIX-SUM COMPACTION (`PSEMU_MESH_LDS_COMPACT=1`).
//
// Ballot tabanli compaction BU MIMARIDE ILKESEL OLARAK GECERSIZ (b5e38d6):
// 64-lane misafir dalga icin `SelectGraphicsLaneMaskMode` bilerek
// PerInvocation seciyor, yani misafir lane'ler host subgroup lane'lerine
// ESLENMIYOR. `OpGroupNonUniformBallot` host subgroup'u okur ve misafir
// dalgayla karsiligi yoktur.
//
// Dogru karsilik: sikistirmayi EMULE EDILEN exec uzerinden, IS GRUBU
// genelinde hesaplamak. LocalSize=64 ve misafir dalga 64 oldugundan
// IS GRUBU = MISAFIR DALGA.
//
// Algoritma (donanimin NGG compaction biriminin yazilim karsiligi):
//   1. her cagri aktiflik durumunu (0/1) LDS'teki kendi yuvasina yazar
//   2. is grubu bariyeri
//   3. kendi indeksinden KUCUK yuvalarin toplami = sikistirilmis indeks
//
// Adim 3 DONGU YERINE ACILMIS (64 yineleme, dallanmasiz OpSelect). Sebep:
// bu fonksiyon zaten yapilandirilmis bir CFG'nin ICINE emit ediliyor; yeni
// bir dongu sokmak bu oturumda defalarca bedeli odenen yapisal kontrol
// akisi risklerini geri getirirdi. Acilmis hali ~256 komut, kabul edilebilir.
//
// TEHLIKE - BARIYER TEKDUZELIGI: `OpControlBarrier` TEKDUZE kontrol
// akisinda olmak zorundadir. Disavurum noktasi lane kapisinin ardinda; tum
// 64 cagri oraya varmiyorsa bariyer KILITLENIR ve GPU asilir (TDR ile
// toparlanir). Bu yuzden VARSAYILAN KAPALI ve bir DENEYDIR.
uint32_t EmitLdsCompactedIndex(EmitterState* state) {
	const auto lane = EmitLocalInvocationIndex(state);
	if (state->mesh_scan_variable == 0) {
		return lane;
	}
	// 1) aktiflik bayragini kendi yuvana yaz
	const auto active   = EmitExecActiveBool(state);
	const auto flag     = state->builder.AllocateId();
	const auto self_ptr = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, flag, active, ConstantU32(state, 1),
	                            ConstantU32(state, 0)});
	state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, self_ptr,
	                            state->mesh_scan_variable, lane});
	state->builder.AddFunction({OpStore, self_ptr, flag});

	// 2) is grubu bariyeri
	state->builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                            ConstantU32(state, ScopeWorkgroup),
	                            ConstantU32(state, MemorySemanticsAcquireRelease |
	                                                   MemorySemanticsWorkgroupMemory)});

	// 3) kendinden kucuk aktif cagrilari say (acilmis, dallanmasiz)
	uint32_t sum = ConstantU32(state, 0);
	for (uint32_t i = 0; i < 64u; i++) {
		const auto slot_ptr = state->builder.AllocateId();
		const auto value    = state->builder.AllocateId();
		const auto before   = state->builder.AllocateId();
		const auto add      = state->builder.AllocateId();
		const auto next     = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, slot_ptr,
		                            state->mesh_scan_variable, ConstantU32(state, i)});
		state->builder.AddFunction({OpLoad, state->uint_type, value, slot_ptr});
		state->builder.AddFunction(
		    {OpULessThan, state->bool_type, before, ConstantU32(state, i), lane});
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, add, before, value, ConstantU32(state, 0)});
		state->builder.AddFunction({OpIAdd, state->uint_type, next, sum, add});
		sum = next;
	}
	static std::atomic<uint32_t> s_l {0};
	if (s_l.fetch_add(1) < 32) {
		std::printf("[MS-LDS-COMPACT] is grubu prefix-toplami kuruldu (64 acilmis yineleme)\n");
		std::fflush(stdout);
	}
	return sum;
}

// ES ISARET TELEMETRISI (`PSEMU_MESH_ES_SENTINEL=1`).
// ES'in LDS yazim noktasina kosulsuz bir isaret birakir; GS tarafinda
// okunur. Olculdu: pozisyonun geldigi LDS bolgesi TAMAMEN BOS (e19ab3c) ve
// ofsetler/siralama STATIK olarak dogru - yani kusur adresleme degil
// YURUTME. Bu sonda "ES blogu hic kosuyor mu" sorusunu dogrudan yanitlar.
bool MeshRingRadarEnabled() {
	static const bool on = [] {
		const char* env = std::getenv("PSEMU_MESH_RING_RADAR");
		return env != nullptr && env[0] == '1';
	}();
	return on;
}

bool MeshRingFixEnabled() {
	static const bool on = [] {
		const char* env = std::getenv("PSEMU_MESH_RING_FIX");
		return env != nullptr && env[0] == '1';
	}();
	return on;
}

bool MeshEsSentinelEnabled() {
	static const bool on = [] {
		const char* env = std::getenv("PSEMU_MESH_ES_SENTINEL");
		return env != nullptr && env[0] == '1';
	}();
	return on;
}

bool MeshLdsCompactionEnabled() {
	static const bool on = [] {
		const char* env = std::getenv("PSEMU_MESH_LDS_COMPACT");
		return env != nullptr && env[0] == '1';
	}();
	return on;
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
// A/B/C/D/E iskeletinin modu; primitif disavurumundan da okunabilsin diye
// dosya duzeyinde.
int MeshSlotTestMode() {
	static const int mode = [] {
		const char* env = std::getenv("PSEMU_MESH_SLOT_TEST");
		return env != nullptr ? std::atoi(env) : 0;
	}();
	return mode;
}

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

	// MOD 6 - INDEKS ARALIGI OKUMASI (tek bitlik, ekrana kodlanmis):
	// oyunun acilan UC indeksi de <= 2 ise BILINEN-KIRMIZI (0,1,2) yaz,
	// aksi halde dejenere (2,2,2) yaz.
	//   KIRMIZI -> indeksler tahsis araliginda (0..2); kusur BASKA yerde
	//   SIYAH   -> en az biri 2'den BUYUK; indeksler sikistirilmis
	//              yuvalarimizin disini gosteriyor
	// Taban cizgi test D (yuva=compaction, kose=HELLO, indeks=0,1,2) KIRMIZI.
	// MOD 7 - AYIRT EDICILIK OKUMASI: mod 6 "hepsi <=2" dedi (KIRMIZI), yani
	// indeksler aralikta. Ama aralikta olmak YETMEZ - ucu de AYNIYSA (orn.
	// (0,0,0)) ilkel dejenere olur ve hicbir fragment cikmaz. Bu kip ucunun
	// FARKLI olup olmadigini sorar.
	//   KIRMIZI -> indeksler farkli; kusur baska yerde
	//   SIYAH   -> indeksler AYNI -> dejenere ilkel; paketli deger muhtemelen
	//              sifir ya da acma tek bir alandan besleniyor
	// MOD 8 - SARIM YONU: mod 6 "hepsi <=2", mod 7 "ucu de FARKLI" dedi -
	// yani indeksler (0,1,2)'nin bir PERMUTASYONU. Buna ragmen mod 5 (gercek
	// indeksler) SIYAH. Tek kalan aciklama: permutasyon TEK (odd) ve sarim
	// yonu ters donuyor, ucgen eleniyor. Bu kip indeksleri TERS yazar.
	//   KIRMIZI -> sorun sarim yonu; ya indeks sirasi ya cull durumu
	//   SIYAH   -> sarim degil; permutasyon disinda bir sey var
	if (MeshSlotTestMode() == 8) {
		const auto vec = state->builder.AllocateId();
		const auto ptr = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vec,
		                            components[2], components[1], components[0]});
		state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, ptr,
		                            state->mesh_prim_indices_variable, index});
		state->builder.AddFunction({OpStore, ptr, vec});
		static std::atomic<uint32_t> s_w {0};
		if (s_w.fetch_add(1) < 32) {
			std::printf("[MS-SARIM] oyunun indeksleri TERS yazildi (c2,c1,c0)\n");
			std::fflush(stdout);
		}
		return;
	}

	if (MeshSlotTestMode() == 7) {
		const auto ne01 = state->builder.AllocateId();
		const auto ne12 = state->builder.AllocateId();
		const auto ne02 = state->builder.AllocateId();
		const auto and1 = state->builder.AllocateId();
		const auto ok   = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, ne01, components[0], components[1]});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, ne12, components[1], components[2]});
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, ne02, components[0], components[2]});
		state->builder.AddFunction({OpLogicalAnd, state->bool_type, and1, ne01, ne12});
		state->builder.AddFunction({OpLogicalAnd, state->bool_type, ok, and1, ne02});
		const auto i1  = state->builder.AllocateId();
		const auto vec = state->builder.AllocateId();
		const auto ptr = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, i1, ok, ConstantU32(state, 1), ConstantU32(state, 2)});
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vec,
		                            ConstantU32(state, 0), i1, ConstantU32(state, 2)});
		state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, ptr,
		                            state->mesh_prim_indices_variable, index});
		state->builder.AddFunction({OpStore, ptr, vec});
		static std::atomic<uint32_t> s_d {0};
		if (s_d.fetch_add(1) < 32) {
			std::printf("[MS-INDEKS-AYIRT] uc indeks FARKLI ise ucgen\n");
			std::fflush(stdout);
		}
		return;
	}

	if (MeshSlotTestMode() == 6) {
		uint32_t in_range = 0;
		for (uint32_t i = 0; i < 3; i++) {
			const auto ok = state->builder.AllocateId();
			state->builder.AddFunction({OpULessThan, state->bool_type, ok, components[i],
			                            ConstantU32(state, 3u)});
			if (i == 0) {
				in_range = ok;
			} else {
				const auto both = state->builder.AllocateId();
				state->builder.AddFunction(
				    {OpLogicalAnd, state->bool_type, both, in_range, ok});
				in_range = both;
			}
		}
		const auto i1  = state->builder.AllocateId();
		const auto i2  = state->builder.AllocateId();
		const auto vec = state->builder.AllocateId();
		const auto ptr = state->builder.AllocateId();
		state->builder.AddFunction({OpSelect, state->uint_type, i1, in_range,
		                            ConstantU32(state, 1), ConstantU32(state, 2)});
		state->builder.AddFunction({OpSelect, state->uint_type, i2, in_range,
		                            ConstantU32(state, 2), ConstantU32(state, 2)});
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vec,
		                            ConstantU32(state, 0), i1, i2});
		state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, ptr,
		                            state->mesh_prim_indices_variable, index});
		state->builder.AddFunction({OpStore, ptr, vec});
		static std::atomic<uint32_t> s_r {0};
		if (s_r.fetch_add(1) < 32) {
			std::printf("[MS-INDEKS-ARALIK] oyunun acilan indeksleri <=2 ise ucgen\n");
			std::fflush(stdout);
		}
		return;
	}

	// SARIM YONU (`PSEMU_MESH_PRIM_TERS=1`).
	//
	// OLCULDU (tek degiskenli, yuva=LDS compaction + kose=HELLO):
	//   indeks = oyunun kendi (c0,c1,c2)  -> SIYAH
	//   indeks = TERS (c2,c1,c0)          -> KIRMIZI
	// Ustteki yorumun kendisi de tutarsiz: paketlemeyi
	// `v12 = (v1<<20)|(v0<<10)|v2` diye TURETIP sonra
	// `i0 | (i1<<10) | (i2<<20)` diye OZETLIYOR - ikisi ayni sey degil.
	// Turetime gore bit0=v2, bit10=v0, bit20=v1; kod ise bit0'i 0. bilesen
	// sayiyor. Yani acilan uclunun SIRASI donanimdakiyle ortusmuyor.
	const bool reverse = [] {
		const char* env = std::getenv("PSEMU_MESH_PRIM_TERS");
		return env != nullptr && env[0] == '1';
	}();
	const auto vector  = state->builder.AllocateId();
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, vector,
	                            reverse ? components[2] : components[0], components[1],
	                            reverse ? components[0] : components[2]});
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
			// ASIMETRIYI KAPAT: NGG'de `exp prim` de tipki `exp pos` gibi
			// OTOMATIK SIKISTIRILIR. Vertex yuvasini LDS compaction'a
			// baglamistik ama primitif yuvasi hala ham lane indeksini
			// kullaniyordu - yani topoloji tahsis sinirinin DISINA
			// yaziliyordu ve rasterizer primitif 0'da cop buluyordu.
			//
			// OLCULDU: yuva=compaction + kose=HELLO + indeks=ZORLA(0,1,2)
			// KIRMIZI (test D), ayni sey indeks=OYUNUN KENDI ile SIYAH
			// (test E). Aradaki tek fark, oyunun indekslerinin OOB bir
			// primitif yuvasina yazilmasiydi.
			//
			// Vertex ve primitif ayni disavurum noktasinda ve ayni exec
			// maskesi altinda oldugu icin AYNI sikistirilmis indeks gecerli.
			EmitPrimitiveIndicesExport(state, inst,
			                           MeshLdsCompactionEnabled()
			                               ? EmitLdsCompactedIndex(state)
			                               : EmitLocalInvocationIndex(state));
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
		// KONUMSAL TELEMETRI (`PSEMU_MESH_TELEMETRY_XY=1`).
		//
		// Tek bitlik (siyah/kirmizi) okumalar yerine ucgeni EKRANDA
		// KONUMLANDIRARAK tek kosuda IKI sayiyi birden okur:
		//   X ekseni -> N = disavurumda hayatta olan toplam cagri sayisi
		//               (ballot(true) + BallotBitCount Reduce)
		//   Y ekseni -> L = ilk aktif lane'in kimligi (BallotFindLSB)
		// Ikisi de alt-grup genelinde TEKDUZE, dolayisiyla tum cagrilar ayni
		// konumu secer; yuva katlamasi (lane%3) ucgeni olusturur.
		//
		// Kova esikleri (sol->sag / alt->ust):
		//   <=1 , <=3 , <=8 , <=32 , >32
		// Konumu okumak icin kareyi merkez/sinir kutusu ile analiz et.
		//
		// Amac: `COMPACT`in neden coktugunu ve `lane%3`un neden calistigini
		// SAYIYLA aciklamak. Lane'ler 0,1,2 degil de 32,33,34 ise Wave64 ile
		// SubgroupSize uyusmazligi kesinlesir.
		// MUTLAK IZOLASYON A/B (`PSEMU_MESH_SLOT_TEST=1|2`).
		//
		// `fc57ef8`de telemetri "N<=1, ilk lane<=1" okudu; ama `6099300`de
		// `yuva=lane` SIYAH, `yuva=lane%3` KIRMIZI cikmisti. Tek aktif lane
		// 0/1 ise iki ifade AYNI yuvayi verir - ikisi ayni anda dogru olamaz.
		// Sebep: o iki kip UC degiskende farkliydi (yuva, primitif indeksler,
		// TAHSIS). Burada ucu de esitlenip YALNIZCA yuva degistiriliyor.
		//
		// Iki testte de ORTAK: tahsis oyunun KENDI m0'i (dokunulmaz),
		// primitif indeksler ZORLA (0,1,2), koseler HELLO koordinatlari.
		//   mod 1 -> outPerVertex[lane]
		//   mod 2 -> outPerVertex[lane % 3]
		//
		// A siyah + B kirmizi  -> telemetri yaniltici; ulasan lane kimlikleri
		//                         3'ten buyuk
		// ikisi ayni           -> telemetri dogru; tek lane ulasiyor ve
		//                         dejenere ilkel bundan
		const int slot_test = MeshSlotTestMode();
		// mod 9 ASAGIDA ele aliniyor (pozisyon saglamlik okumasi); burada
		// yakalanirsa erken return yuzunden OLU KOD olur - olculdu.
		if (mesh && slot_test > 0 && slot_test != 9 && slot_test != 10 && slot_test != 11 && slot_test != 12 && slot_test != 13 && slot_test != 14 && slot_test != 15 && slot_test != 16) {
			const auto lane_id = EmitLocalInvocationIndex(state);
			uint32_t   slot    = lane_id;
			if (slot_test == 2) {
				const auto folded = state->builder.AllocateId();
				state->builder.AddFunction(
				    {OpUMod, state->uint_type, folded, lane_id, ConstantU32(state, 3u)});
				slot = folded;
			} else if (slot_test == 3) {
				// TEST C: donanimin NGG vertex compaction'i - aktif lane'ler
				// arasinda kacinci oldugumuz (ballot(true) + ExclusiveScan).
				slot = EmitNggCompactedIndex(state);
			} else if (slot_test >= 4) {
				// TEST D/E: LDS prefix-toplami ile is grubu genelinde compaction.
				slot = EmitLdsCompactedIndex(state);
			}
			const auto value    = EmitFixedTrianglePosition(state, lane_id);
			const auto slot_ptr = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, slot_ptr,
			                            variable, slot, ConstantU32(state, 0)});
			state->builder.AddFunction({OpStore, slot_ptr, value});
			// TEST E (mod 5): indeksleri ZORLAMA - oyunun KENDI 3x10-bit
			// paketinden acilan indeksleri birak. Boylece "indeks acma dogru
			// mu" sorusu pozisyon degerlerinden AYRISIR:
			//   KIRMIZI -> indeks acma saglam, kusur pozisyon matematiginde
			//   SIYAH   -> kusur indekslerde (acma yanlis ya da indeksler
			//              sikistirma sirasiyla ortusmuyor)
			if (slot_test != 5 && state->mesh_prim_indices_variable != 0) {
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
			static std::atomic<uint32_t> s_s {0};
			if (s_s.fetch_add(1) < 32) {
				std::printf("[MS-SLOT-TEST] mod=%d yuva=%s (tahsis=oyunun m0, indeks=%s, "
				            "kose=HELLO) pc=0x%08x\n",
				            slot_test,
				            slot_test == 5   ? "lds-compaction+OYUN-INDEKSI"
				            : slot_test == 4 ? "lds-compaction"
				            : slot_test == 3 ? "ballot-compaction"
				            : slot_test == 2 ? "lane%3"
				                             : "lane",
				            slot_test == 5 ? "OYUNUN-KENDI" : "0/1/2", inst.pc);
				std::fflush(stdout);
			}
			return;
		}
		static const bool telemetry_xy = [] {
			const char* env = std::getenv("PSEMU_MESH_TELEMETRY_XY");
			return env != nullptr && env[0] == '1';
		}();
		if (mesh && telemetry_xy) {
			const auto true_id = state->builder.AllocateId();
			const auto ballot  = state->builder.AllocateId();
			const auto n_count = state->builder.AllocateId();
			const auto first   = state->builder.AllocateId();
			state->builder.AddFunction({OpINotEqual, state->bool_type, true_id,
			                            ConstantU32(state, 1), ConstantU32(state, 0)});
			state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
			                            ConstantU32(state, ScopeSubgroup), true_id});
			state->builder.AddFunction({OpGroupNonUniformBallotBitCount, state->uint_type, n_count,
			                            ConstantU32(state, ScopeSubgroup), GroupOperationReduce,
			                            ballot});
			state->builder.AddFunction({OpGroupNonUniformBallotFindLSB, state->uint_type, first,
			                            ConstantU32(state, ScopeSubgroup), ballot});

			// Degeri bes kovaya ayirip [-0.7, +0.7] arasinda bir ofsete cevir.
			const auto bucket = [&](uint32_t value) {
				const float slots[5] = {-0.7f, -0.35f, 0.0f, 0.35f, 0.7f};
				uint32_t    result   = ConstantF32Value(state, slots[4]);
				const uint32_t limits[4] = {32u, 8u, 3u, 1u};
				for (int i = 0; i < 4; i++) {
					const auto cmp = state->builder.AllocateId();
					const auto sel = state->builder.AllocateId();
					state->builder.AddFunction({OpUGreaterThan, state->bool_type, cmp, value,
					                            ConstantU32(state, limits[i])});
					state->builder.AddFunction({OpSelect, state->float_type, sel, cmp,
					                            ConstantF32Value(state, slots[4 - i]),
					                            ConstantF32Value(state, slots[3 - i])});
					result = sel;
				}
				return result;
			};
			const auto x_off = bucket(n_count);
			const auto y_off = bucket(first);

			const float corners[3][2] = {{-0.12f, -0.12f}, {0.12f, -0.12f}, {0.0f, 0.12f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto cy  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpFAdd, state->float_type, cy, y_off,
				                            ConstantF32Value(state, corners[i][1])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            cy, ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_x {0};
			if (s_x.fetch_add(1) < 32) {
				std::printf("[MS-TELEMETRI-XY] X=aktif_sayi Y=ilk_lane kovalari "
				            "(-0.7,-0.35,0,0.35,0.7) esikler(1,3,8,32) pc=0x%08x\n",
				            inst.pc);
				std::fflush(stdout);
			}
			return;
		}
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
		// MOD 9 - POZISYON SAGLAMLIK OKUMASI (`PSEMU_MESH_SLOT_TEST=9`).
		//
		// Uc kusurdan ikisi kapandi (yuva indeksi -> LDS compaction; primitif
		// indeks sirasi -> ters cevirme). Oyunun KENDI pozisyonlariyla ekran
		// hala siyah. Rasterizer bir ucgeni yalnizca su hallerde atar:
		//   w <= 0        -> homojen bolme patlar, frustum culling atar
		//   NaN / Inf     -> hesap gecersiz
		//   NDC disi      -> tamamen gorus alaninin disinda
		//
		// Sonucu RENKLE degil KONUMLA sinyalliyoruz: PS_FIXCOLOR zaten her
		// seyi kirmizi yaptigi icin renk kanali guvenilmez.
		//   ucgen SOLDA  -> pozisyon BOZUK
		//   ucgen SAGDA  -> pozisyon SAGLAM
		// NaN ortuk olarak yakalaniyor: NaN'da TUM sirali (Ord) kiyaslar
		// false doner, yani "bozuk" tarafina duser.
		// MOD 10 - BOZUKLUGUN KARAKTERI (dort kademeli konum).
		// Mod 9 "bozuk" dedi; bu kip HANGI kosulun ihlal edildigini ayirir.
		// Konumlar (soldan saga): NaN | w<=0 | |x|>4w | |y|>4w | saglam
		// NaN, `x != x` (FUnordNotEqual) ile ACIKCA yakalaniyor.
		//
		// SELECT ZINCIRI DOGRU KURULDU: her adim BIR ONCEKI sonucu taban alir.
		// Onceki konumsal telemetride (fc57ef8) her adim `result`u uzerine
		// yazmis ve yalnizca SON kiyas hayatta kalmisti - o hata bir olcumu
		// gecersiz kildi.
		// MOD 11 - "LDS BOS MU, YOKSA YALNIZCA w MI SIFIR?"
		// Pozisyon LDS'ten geliyor (ES->GS devri) ve w<=0 olculdu. Ofsetler ve
		// siralama STATIK olarak dogru cikti (ES 128/132/136/140'a yaziyor, GS
		// ayni ofsetlerden okuyor, yazimlar okumalardan once, 5 bariyer var).
		// Geriye iki ihtimal kaldi ve bu kip onlari ayirir:
		//   x=y=z=w=0     -> LDS bolgesine HIC yazilmamis (ES kapisi kapali)
		//   w=0, digerleri-> yazma OLMUS ama w degeri sifir (deger/kaynak hatasi)
		// Konumlar: -0.6=hepsi sifir | 0.0=yalnizca w sifir | 0.6=w dolu
		// MOD 12 - ES ISARETI OKUMASI.
		// ES'in LDS yazim noktasina birakilan isareti GS'te oku:
		//   SAG  (+0.6) -> isaret ULASTI  => ES blogu KOSUYOR; pozisyonlarin
		//                  sifir olmasi baska bir sebepten (deger/ofset kaymasi)
		//   SOL  (-0.6) -> isaret YOK     => ES blogu hic yurutulmuyor
		//                  (thread/lane kapisi yutuyor)
		// MOD 13 - ZAMANLAMA OKUMASI: ILK LDS okumasi aninda ES isareti var
		// miydi? (scan[65]; LDS okuma yolunda yakalaniyor)
		//   -0.6 = hic LDS okumasi olmadi
		//    0.0 = ILK okumada isaret YOKTU -> ES yazimi okumadan SONRA
		//          (SPLICE SIRALAMA HATASI)
		//    0.6 = ILK okumada isaret VARDI -> sira dogru, kusur baska yerde
		// MOD 14 - RADAR OKUMASI: bookkeeping okumasinin GERCEK indeksi ile
		// teorik `40 + 12*lane` degeri uyusuyor mu?
		//   -0.6 = o okuma HIC yapilmadi (yakalama tutmadi)
		//    0.0 = yapildi ama indeks BEKLENENDEN FARKLI -> teori yanlis
		//    0.6 = indeks TAM beklendigi gibi -> teori dogru, kusur baska yerde
		// MOD 15 - DELTA RADARI (iki eksenli).
		// scan[66] = gercek dword indeksi + 1, scan[67] = (40 + 12*lane) + 1
		//   X ekseni = GERCEK INDEKSIN buyuklugu
		//     -0.7 = okuma yok | -0.35 = <64 | 0.0 = <1024
		//      0.35 = <65536   |  0.7 = daha buyuk (cop suphesi)
		//   Y ekseni = DELTA'nin buyuklugu (got - want, isaretsiz fark)
		//     -0.7 = 0 | -0.35 = <64 | 0.0 = <1024 | 0.35 = <65536 | 0.7 = dev
		// Sabit bir kayma -> kucuk delta kovasi; lane'e orantili ya da cop ->
		// buyuk kovalar.
		// MOD 16 - ANALOG OKUMA (tek ucgen, iki eksen).
		// Kova yerine SAYININ KENDISI: deger konuma dogrusal gomuluyor.
		//   eksen = -0.9 + min(deger, 4095)/4095 * 1.8
		//   X = GS'in OKUDUGU dword indeksi  (scan[69])
		//   Y = ES'in  YAZDIGI dword indeksi (scan[70])
		// Ikisi de LANE 0 kanali - ayni invocation, karsilastirma birebir.
		// Mevcut tahsis 3 kose / 1 primitif oldugu icin iki satir yerine iki
		// EKSEN kullaniyoruz; boylece SetMeshOutputsEXT'e dokunmuyoruz.
		// Yuva bosken (0) eksen -0.95'e, skalanin disina dusurulur.
		if (mesh && MeshSlotTestMode() == 16 && state->mesh_scan_variable != 0) {
			const auto axis = [&](uint32_t slot) {
				const auto ptr   = state->builder.AllocateId();
				const auto raw   = state->builder.AllocateId();
				const auto seen  = state->builder.AllocateId();
				const auto val   = state->builder.AllocateId();
				const auto capc  = state->builder.AllocateId();
				const auto capv  = state->builder.AllocateId();
				const auto fval  = state->builder.AllocateId();
				const auto scale = state->builder.AllocateId();
				const auto onsc  = state->builder.AllocateId();
				const auto out   = state->builder.AllocateId();
				state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, ptr,
				                            state->mesh_scan_variable, ConstantU32(state, slot)});
				state->builder.AddFunction({OpLoad, state->uint_type, raw, ptr});
				// Sentinel'ler giris blogunda yazildigi icin "yazilmadi" hali
				// artik 0 DEGIL: min yuvasi 0xFFFFFFFF kalmissa hic yazim
				// olmamis demektir. Degerler HAM indeks, arti bir yok.
				state->builder.AddFunction({OpULessThan, state->bool_type, seen, raw,
				                            ConstantU32(state, 0xFFFFFFFFu)});
				state->builder.AddFunction(
				    {OpBitwiseOr, state->uint_type, val, raw, ConstantU32(state, 0)});
				state->builder.AddFunction(
				    {OpULessThan, state->bool_type, capc, val, ConstantU32(state, 4095u)});
				state->builder.AddFunction({OpSelect, state->uint_type, capv, capc, val,
				                            ConstantU32(state, 4095u)});
				state->builder.AddFunction({OpConvertUToF, state->float_type, fval, capv});
				state->builder.AddFunction({OpFMul, state->float_type, scale, fval,
				                            ConstantF32Value(state, 1.8f / 4095.0f)});
				state->builder.AddFunction({OpFAdd, state->float_type, onsc, scale,
				                            ConstantF32Value(state, -0.9f)});
				state->builder.AddFunction({OpSelect, state->float_type, out, seen, onsc,
				                            ConstantF32Value(state, -0.95f)});
				return std::pair<uint32_t, uint32_t> {out, seen};
			};
			const auto [x_off, x_seen] = axis(72u);   // dword 40'a TAM ISABET sayisi
			const auto [y_off, y_seen] = axis(73u);   // lane 0 TOPLAM LDS yazimi
			// HIC OLCUM YAPMAMIS shader ucgen CIZMESIN. 56 mesh shader var ve
			// birden fazlasi bu okuma blogundan geciyor; olcumsuzler skalanin
			// disina ucgen basarsa merkez ORTALAMAYA kayar ve sayi okunamaz.
			// Cozum: iki yuva da bossa ucgeni dejenere et (uc kose ayni nokta),
			// boylece hic piksel uretmez ve olcum yapanlar yalniz kalir.
			// NOT: "olcum yoksa dejenere ucgen" denendi ve GERI ALINDI - ekrana
			// ulasan shader'lari da sustururken geriye yalnizca ekrana hic
			// varmayan bir ucgen kaldi, ustunu de oyunun tam ekran gecisi
			// ortti. Olcumsuzler yine -0.95 kosesine cizer; ayirt etme isi
			// okuyucuda (baglantili bilesen), boylece ortalama alinmaz.
			(void)x_seen;
			(void)y_seen;
			const float corners[3][2] = {{-0.04f, -0.04f}, {0.04f, -0.04f}, {0.0f, 0.04f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto cy  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto vp  = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpFAdd, state->float_type, cy, y_off,
				                            ConstantF32Value(state, corners[i][1])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            cy, ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, vp,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, vp, vec});
			}
			if (state->mesh_prim_indices_variable != 0) {
				const auto tri = state->builder.AllocateId();
				const auto tp  = state->builder.AllocateId();
				state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, tri,
				                            ConstantU32(state, 0), ConstantU32(state, 1),
				                            ConstantU32(state, 2)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec3_uint, tp,
				                            state->mesh_prim_indices_variable,
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, tp, tri});
			}
			static std::atomic<uint32_t> s_an {0};
			if (s_an.fetch_add(1) < 32) {
				std::printf("[MS-ANALOG] X = GS okuma indeksi, Y = ES yazma indeksi (lane 0)\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 15 && state->mesh_scan_variable != 0) {
			// scan[66] = 1 + (lane<<24 | indeks). Tek store oldugu icin lane ve
			// indeks KESINLIKLE ayni invocation'a ait; beklenen degeri de o
			// lane'den uretiyoruz, yani karsilastirma lane-ici.
			const auto pg   = state->builder.AllocateId();
			const auto raw  = state->builder.AllocateId();
			const auto seen = state->builder.AllocateId();
			const auto pk   = state->builder.AllocateId();
			const auto ln   = state->builder.AllocateId();
			const auto ix   = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, pg,
			                            state->mesh_scan_variable, ConstantU32(state, 66u)});
			state->builder.AddFunction({OpLoad, state->uint_type, raw, pg});
			state->builder.AddFunction(
			    {OpINotEqual, state->bool_type, seen, raw, ConstantU32(state, 0)});
			state->builder.AddFunction(
			    {OpISub, state->uint_type, pk, raw, ConstantU32(state, 1u)});
			state->builder.AddFunction({OpShiftRightLogical, state->uint_type, ln, pk,
			                            ConstantU32(state, 24u)});
			state->builder.AddFunction({OpBitwiseAnd, state->uint_type, ix, pk,
			                            ConstantU32(state, 0xFFFFFFu)});
			// beklenen = 40 + 12*lane, ayni lane'den
			const auto l12  = state->builder.AllocateId();
			const auto want = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpIMul, state->uint_type, l12, ln, ConstantU32(state, 12u)});
			state->builder.AddFunction(
			    {OpIAdd, state->uint_type, want, l12, ConstantU32(state, 40u)});
			// mutlak fark
			const auto ge   = state->builder.AllocateId();
			const auto d1   = state->builder.AllocateId();
			const auto d2   = state->builder.AllocateId();
			const auto diff = state->builder.AllocateId();
			state->builder.AddFunction({OpUGreaterThanEqual, state->bool_type, ge, ix, want});
			state->builder.AddFunction({OpISub, state->uint_type, d1, ix, want});
			state->builder.AddFunction({OpISub, state->uint_type, d2, want, ix});
			state->builder.AddFunction({OpSelect, state->uint_type, diff, ge, d1, d2});
			// "hic okunmadi" durumunu X ekseninde ayirt edebilmek icin: okuma
			// olmadiysa indeksi 0'a zorla (kova -0.7), delta'yi da 0'a.
			const auto ix1 = state->builder.AllocateId();
			const auto g1  = state->builder.AllocateId();
			const auto df0 = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpIAdd, state->uint_type, ix1, ix, ConstantU32(state, 1u)});
			state->builder.AddFunction({OpSelect, state->uint_type, g1, seen, ix1,
			                            ConstantU32(state, 0)});
			state->builder.AddFunction({OpSelect, state->uint_type, df0, seen, diff,
			                            ConstantU32(state, 0)});
			// kova zinciri: her adim BIR ONCEKI sonucu taban alir
			const auto bucket = [&](uint32_t value, bool zero_is_first) {
				uint32_t   res      = ConstantF32Value(state, 0.7f);
				const uint32_t lim[3] = {65536u, 1024u, 64u};
				const float    pos[3] = {0.35f, 0.0f, -0.35f};
				for (int i = 0; i < 3; i++) {
					const auto c = state->builder.AllocateId();
					const auto r = state->builder.AllocateId();
					state->builder.AddFunction({OpULessThan, state->bool_type, c, value,
					                            ConstantU32(state, lim[i])});
					state->builder.AddFunction({OpSelect, state->float_type, r, c,
					                            ConstantF32Value(state, pos[i]), res});
					res = r;
				}
				if (zero_is_first) {
					const auto z  = state->builder.AllocateId();
					const auto rz = state->builder.AllocateId();
					state->builder.AddFunction(
					    {OpIEqual, state->bool_type, z, value, ConstantU32(state, 0)});
					state->builder.AddFunction({OpSelect, state->float_type, rz, z,
					                            ConstantF32Value(state, -0.7f), res});
					res = rz;
				}
				return res;
			};
			const auto x_off = bucket(g1, true);
			const auto y_off = bucket(df0, true);
			const float corners[3][2] = {{-0.1f, -0.1f}, {0.1f, -0.1f}, {0.0f, 0.1f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto cy  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpFAdd, state->float_type, cy, y_off,
				                            ConstantF32Value(state, corners[i][1])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            cy, ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_dl {0};
			if (s_dl.fetch_add(1) < 32) {
				std::printf("[MS-DELTA] X=gercek indeks buyuklugu Y=delta buyuklugu "
				            "(kovalar: yok/<64/<1024/<65536/dev)\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 14 && state->mesh_scan_variable != 0) {
			const auto pg = state->builder.AllocateId();
			const auto pw = state->builder.AllocateId();
			const auto g  = state->builder.AllocateId();
			const auto w  = state->builder.AllocateId();
			const auto none = state->builder.AllocateId();
			const auto same = state->builder.AllocateId();
			const auto s1 = state->builder.AllocateId();
			const auto x_off = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, pg,
			                            state->mesh_scan_variable, ConstantU32(state, 66u)});
			state->builder.AddFunction({OpLoad, state->uint_type, g, pg});
			state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, pw,
			                            state->mesh_scan_variable, ConstantU32(state, 67u)});
			state->builder.AddFunction({OpLoad, state->uint_type, w, pw});
			state->builder.AddFunction(
			    {OpIEqual, state->bool_type, none, g, ConstantU32(state, 0)});
			state->builder.AddFunction({OpIEqual, state->bool_type, same, g, w});
			state->builder.AddFunction({OpSelect, state->float_type, s1, same,
			                            ConstantF32Value(state, 0.6f),
			                            ConstantF32Value(state, 0.0f)});
			state->builder.AddFunction({OpSelect, state->float_type, x_off, none,
			                            ConstantF32Value(state, -0.6f), s1});
			const float corners[3][2] = {{-0.12f, -0.12f}, {0.12f, -0.12f}, {0.0f, 0.12f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_rd {0};
			if (s_rd.fetch_add(1) < 32) {
				std::printf("[MS-RADAR-OKU] -0.6=okuma yok 0.0=indeks FARKLI 0.6=indeks AYNI\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 13 && state->mesh_scan_variable != 0) {
			const auto ptr_s = state->builder.AllocateId();
			const auto val   = state->builder.AllocateId();
			const auto is1   = state->builder.AllocateId();
			const auto is2   = state->builder.AllocateId();
			const auto s1    = state->builder.AllocateId();
			const auto x_off = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, ptr_s,
			                            state->mesh_scan_variable, ConstantU32(state, 65u)});
			state->builder.AddFunction({OpLoad, state->uint_type, val, ptr_s});
			state->builder.AddFunction(
			    {OpIEqual, state->bool_type, is1, val, ConstantU32(state, 1)});
			state->builder.AddFunction(
			    {OpIEqual, state->bool_type, is2, val, ConstantU32(state, 2)});
			state->builder.AddFunction({OpSelect, state->float_type, s1, is2,
			                            ConstantF32Value(state, 0.6f),
			                            ConstantF32Value(state, -0.6f)});
			state->builder.AddFunction({OpSelect, state->float_type, x_off, is1,
			                            ConstantF32Value(state, 0.0f), s1});
			const float corners[3][2] = {{-0.12f, -0.12f}, {0.12f, -0.12f}, {0.0f, 0.12f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_t {0};
			if (s_t.fetch_add(1) < 32) {
				std::printf("[MS-ZAMANLAMA] -0.6=LDS okumasi yok 0.0=ILK okumada isaret YOK "
				            "0.6=isaret VARDI\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 12 && state->mesh_scan_variable != 0) {
			const auto ptr_s = state->builder.AllocateId();
			const auto mark  = state->builder.AllocateId();
			const auto seen  = state->builder.AllocateId();
			const auto x_off = state->builder.AllocateId();
			state->builder.AddFunction({OpAccessChain, state->ptr_mesh_scan_uint, ptr_s,
			                            state->mesh_scan_variable, ConstantU32(state, 64u)});
			state->builder.AddFunction({OpLoad, state->uint_type, mark, ptr_s});
			state->builder.AddFunction(
			    {OpINotEqual, state->bool_type, seen, mark, ConstantU32(state, 0)});
			state->builder.AddFunction({OpSelect, state->float_type, x_off, seen,
			                            ConstantF32Value(state, 0.6f),
			                            ConstantF32Value(state, -0.6f)});
			const float corners[3][2] = {{-0.12f, -0.12f}, {0.12f, -0.12f}, {0.0f, 0.12f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_e {0};
			if (s_e.fetch_add(1) < 32) {
				std::printf("[MS-ES-ISARET] SAG=isaret ulasti (ES kosuyor) SOL=ulasmadi\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 11) {
			uint32_t comp[4] = {};
			for (uint32_t i = 0; i < 4; i++) {
				comp[i] = state->builder.AllocateId();
				state->builder.AddFunction(
				    {OpCompositeExtract, state->float_type, comp[i], value, i});
			}
			const auto zero = ConstantF32Value(state, 0.0f);
			uint32_t   z[4] = {};
			for (uint32_t i = 0; i < 4; i++) {
				z[i] = state->builder.AllocateId();
				state->builder.AddFunction({OpFOrdEqual, state->bool_type, z[i], comp[i], zero});
			}
			const auto z01 = state->builder.AllocateId();
			const auto z012 = state->builder.AllocateId();
			const auto all_zero = state->builder.AllocateId();
			state->builder.AddFunction({OpLogicalAnd, state->bool_type, z01, z[0], z[1]});
			state->builder.AddFunction({OpLogicalAnd, state->bool_type, z012, z01, z[2]});
			state->builder.AddFunction({OpLogicalAnd, state->bool_type, all_zero, z012, z[3]});
			// zincir: taban "w dolu", sirayla ozel durumlar bastirir
			const auto s1 = state->builder.AllocateId();
			const auto s2 = state->builder.AllocateId();
			state->builder.AddFunction({OpSelect, state->float_type, s1, z[3],
			                            ConstantF32Value(state, 0.0f),
			                            ConstantF32Value(state, 0.6f)});
			state->builder.AddFunction({OpSelect, state->float_type, s2, all_zero,
			                            ConstantF32Value(state, -0.6f), s1});
			const float corners[3][2] = {{-0.12f, -0.12f}, {0.12f, -0.12f}, {0.0f, 0.12f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, s2,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_z {0};
			if (s_z.fetch_add(1) < 32) {
				std::printf("[MS-LDS-BOS] -0.6=hepsi sifir 0.0=yalnizca w sifir 0.6=w dolu\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 10) {
			const auto px = state->builder.AllocateId();
			const auto py = state->builder.AllocateId();
			const auto pw = state->builder.AllocateId();
			state->builder.AddFunction({OpCompositeExtract, state->float_type, px, value, 0});
			state->builder.AddFunction({OpCompositeExtract, state->float_type, py, value, 1});
			state->builder.AddFunction({OpCompositeExtract, state->float_type, pw, value, 3});
			const auto nx = state->builder.AllocateId();
			const auto ny = state->builder.AllocateId();
			const auto nw = state->builder.AllocateId();
			const auto n1 = state->builder.AllocateId();
			const auto is_nan = state->builder.AllocateId();
			state->builder.AddFunction({OpFUnordNotEqual, state->bool_type, nx, px, px});
			state->builder.AddFunction({OpFUnordNotEqual, state->bool_type, ny, py, py});
			state->builder.AddFunction({OpFUnordNotEqual, state->bool_type, nw, pw, pw});
			state->builder.AddFunction({OpLogicalOr, state->bool_type, n1, nx, ny});
			state->builder.AddFunction({OpLogicalOr, state->bool_type, is_nan, n1, nw});
			const auto w_bad = state->builder.AllocateId();
			state->builder.AddFunction({OpFOrdLessThanEqual, state->bool_type, w_bad, pw,
			                            ConstantF32Value(state, 0.0f)});
			const auto ax    = state->builder.AllocateId();
			const auto ay    = state->builder.AllocateId();
			const auto bound = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpExtInst, state->float_type, ax, state->glsl_std450, GlslFAbs, px});
			state->builder.AddFunction(
			    {OpExtInst, state->float_type, ay, state->glsl_std450, GlslFAbs, py});
			state->builder.AddFunction({OpFMul, state->float_type, bound, pw,
			                            ConstantF32Value(state, 4.0f)});
			const auto x_bad = state->builder.AllocateId();
			const auto y_bad = state->builder.AllocateId();
			state->builder.AddFunction({OpFOrdGreaterThan, state->bool_type, x_bad, ax, bound});
			state->builder.AddFunction({OpFOrdGreaterThan, state->bool_type, y_bad, ay, bound});

			uint32_t   x_off  = ConstantF32Value(state, 0.8f); // saglam
			const auto s_y    = state->builder.AllocateId();
			const auto s_x    = state->builder.AllocateId();
			const auto s_w    = state->builder.AllocateId();
			const auto s_n    = state->builder.AllocateId();
			state->builder.AddFunction({OpSelect, state->float_type, s_y, y_bad,
			                            ConstantF32Value(state, 0.4f), x_off});
			state->builder.AddFunction({OpSelect, state->float_type, s_x, x_bad,
			                            ConstantF32Value(state, 0.0f), s_y});
			state->builder.AddFunction({OpSelect, state->float_type, s_w, w_bad,
			                            ConstantF32Value(state, -0.4f), s_x});
			state->builder.AddFunction({OpSelect, state->float_type, s_n, is_nan,
			                            ConstantF32Value(state, -0.8f), s_w});
			x_off = s_n;

			const float corners[3][2] = {{-0.12f, -0.12f}, {0.12f, -0.12f}, {0.0f, 0.12f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_k {0};
			if (s_k.fetch_add(1) < 32) {
				std::printf("[MS-POZ-KARAKTER] -0.8=NaN -0.4=w<=0 0.0=|x|>4w 0.4=|y|>4w "
				            "0.8=saglam\n");
				std::fflush(stdout);
			}
			return;
		}

		if (mesh && MeshSlotTestMode() == 9) {
			const auto px = state->builder.AllocateId();
			const auto py = state->builder.AllocateId();
			const auto pw = state->builder.AllocateId();
			state->builder.AddFunction({OpCompositeExtract, state->float_type, px, value, 0});
			state->builder.AddFunction({OpCompositeExtract, state->float_type, py, value, 1});
			state->builder.AddFunction({OpCompositeExtract, state->float_type, pw, value, 3});
			const auto zero  = ConstantF32Value(state, 0.0f);
			const auto w_pos = state->builder.AllocateId();
			state->builder.AddFunction({OpFOrdGreaterThan, state->bool_type, w_pos, pw, zero});
			const auto ax    = state->builder.AllocateId();
			const auto ay    = state->builder.AllocateId();
			const auto bound = state->builder.AllocateId();
			state->builder.AddFunction({OpExtInst, state->float_type, ax, state->glsl_std450,
			                            GlslFAbs, px});
			state->builder.AddFunction({OpExtInst, state->float_type, ay, state->glsl_std450,
			                            GlslFAbs, py});
			state->builder.AddFunction({OpFMul, state->float_type, bound, pw,
			                            ConstantF32Value(state, 4.0f)});
			const auto x_ok  = state->builder.AllocateId();
			const auto y_ok  = state->builder.AllocateId();
			const auto and1  = state->builder.AllocateId();
			const auto valid = state->builder.AllocateId();
			state->builder.AddFunction({OpFOrdLessThanEqual, state->bool_type, x_ok, ax, bound});
			state->builder.AddFunction({OpFOrdLessThanEqual, state->bool_type, y_ok, ay, bound});
			state->builder.AddFunction({OpLogicalAnd, state->bool_type, and1, w_pos, x_ok});
			state->builder.AddFunction({OpLogicalAnd, state->bool_type, valid, and1, y_ok});
			const auto x_off = state->builder.AllocateId();
			state->builder.AddFunction({OpSelect, state->float_type, x_off, valid,
			                            ConstantF32Value(state, 0.5f),
			                            ConstantF32Value(state, -0.5f)});
			const float corners[3][2] = {{-0.25f, -0.25f}, {0.25f, -0.25f}, {0.0f, 0.25f}};
			for (uint32_t i = 0; i < 3; i++) {
				const auto cx  = state->builder.AllocateId();
				const auto vec = state->builder.AllocateId();
				const auto ptr = state->builder.AllocateId();
				state->builder.AddFunction({OpFAdd, state->float_type, cx, x_off,
				                            ConstantF32Value(state, corners[i][0])});
				state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, vec, cx,
				                            ConstantF32Value(state, corners[i][1]),
				                            ConstantF32Value(state, 0.0f),
				                            ConstantF32Value(state, 1.0f)});
				state->builder.AddFunction({OpAccessChain, state->ptr_output_vec4_float, ptr,
				                            variable, ConstantU32(state, i),
				                            ConstantU32(state, 0)});
				state->builder.AddFunction({OpStore, ptr, vec});
			}
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
			static std::atomic<uint32_t> s_p {0};
			if (s_p.fetch_add(1) < 32) {
				std::printf("[MS-POZ-SAGLAMLIK] SOL=bozuk SAG=saglam (w>0 && |x|<=4w && |y|<=4w)\n");
				std::fflush(stdout);
			}
			return;
		}

		const auto pointer = state->builder.AllocateId();
		if (mesh) {
			// NGG vertex compaction: donanim `exp pos0`da hayatta kalan
			// lane'leri otomatik sikistirir. Emulator 1:1 esleme yapiyordu.
			// LDS yolu TEST D'de KIRMIZI verdi (is grubu prefix-toplami);
			// ballot yolu bu mimaride gecersiz (bkz. b5e38d6).
			const auto lane = MeshLdsCompactionEnabled() ? EmitLdsCompactedIndex(state)
			                  : MeshCompactionEnabled()  ? EmitNggCompactedIndex(state)
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
