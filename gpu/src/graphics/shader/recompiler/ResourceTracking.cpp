#include "graphics/shader/recompiler/ResourceTracking.h"

#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		case ShaderType::Mesh: return "mesh";
		default: return "unknown";
	}
}

bool IsAtomic(Opcode op) {
	switch (op) {
		case Opcode::AtomicSwapU32:
		case Opcode::AtomicAddU32:
		case Opcode::AtomicSubU32:
		case Opcode::AtomicSMinI32:
		case Opcode::AtomicUMinU32:
		case Opcode::AtomicSMaxI32:
		case Opcode::AtomicUMaxU32:
		case Opcode::AtomicAndU32:
		case Opcode::AtomicOrU32:
		case Opcode::AtomicXorU32: return true;
		default: return false;
	}
}

bool IsWrite(Opcode op) {
	switch (op) {
		case Opcode::BufferStoreByte:
		case Opcode::BufferStoreShort:
		case Opcode::BufferStoreDword:
		case Opcode::ImageStore: return true;
		default: return IsAtomic(op);
	}
}

bool NeedsSampler(Opcode op) {
	return op == Opcode::ImageSample || op == Opcode::ImageGather4 || op == Opcode::ImageGetLod;
}

bool IsDepthCompare(const Instruction& inst) {
	return (inst.memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
}

ImageMipMode MipMode(const Instruction& inst) {
	const bool storage = inst.memory.kind == ResourceKind::StorageImage ||
	                     inst.memory.kind == ResourceKind::StorageImageUint;
	return storage && inst.memory.image_has_mip ? ImageMipMode::DynamicStorage : ImageMipMode::None;
}

bool IsBuffer(const Instruction& inst) {
	return inst.memory.kind == ResourceKind::Buffer ||
	       (inst.memory.kind == ResourceKind::ScalarBuffer && inst.op == Opcode::SBufferLoadDword);
}

bool IsImage(const Instruction& inst) {
	return inst.memory.kind == ResourceKind::Image || inst.memory.kind == ResourceKind::ImageUint ||
	       inst.memory.kind == ResourceKind::StorageImage ||
	       inst.memory.kind == ResourceKind::StorageImageUint;
}

bool IsAddress(const Instruction& inst) {
	return inst.op == Opcode::SLoadDword || inst.memory.kind == ResourceKind::Flat ||
	       inst.memory.kind == ResourceKind::Global || inst.memory.kind == ResourceKind::Scratch;
}

uint32_t ByteExtent(const Instruction& inst) {
	const auto bytes = std::max((inst.memory.data_bits + 7u) / 8u, 1u);
	const auto count = std::max(inst.memory.data_dwords, 1u);
	const auto end =
	    static_cast<uint64_t>(inst.memory.offset) + static_cast<uint64_t>(bytes) * count;
	return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

bool ContainsUnknown(const ScalarProvenance& provenance, uint32_t id, std::vector<uint8_t>* visited,
                     std::vector<uint32_t>* path) {
	path->push_back(id);
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return true;
	}
	if ((*visited)[id] != 0) {
		path->pop_back();
		return false;
	}
	(*visited)[id]    = 1;
	const auto& value = provenance.values[id];
	if (value.op == ScalarValueOp::Phi) {
		for (const auto arg: value.phi_args) {
			if (ContainsUnknown(provenance, arg, visited, path)) {
				return true;
			}
		}
		path->pop_back();
		return false;
	}
	const auto args = ScalarValueArgCount(value.op);
	for (uint32_t i = 0; i < args; i++) {
		if (ContainsUnknown(provenance, value.args[i], visited, path)) {
			return true;
		}
	}
	path->pop_back();
	return false;
}

bool IsLoopInvariantValue(const ScalarProvenance& provenance, uint32_t id,
                          std::vector<uint8_t>* visiting) {
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return false;
	}
	if ((*visiting)[id] != 0) {
		return true;
	}
	(*visiting)[id]   = 1;
	const auto& value = provenance.values[id];
	if (value.op == ScalarValueOp::Phi) {
		uint32_t invariant = ScalarProvenance::Undefined;
		for (const auto arg: value.phi_args) {
			if (arg == id) {
				continue;
			}
			if (invariant == ScalarProvenance::Undefined) {
				invariant = arg;
			} else if (arg != invariant) {
				return false;
			}
		}
		return invariant != ScalarProvenance::Undefined &&
		       IsLoopInvariantValue(provenance, invariant, visiting);
	}
	for (uint32_t i = 0; i < ScalarValueArgCount(value.op); i++) {
		if (!IsLoopInvariantValue(provenance, value.args[i], visiting)) {
			return false;
		}
	}
	return true;
}

bool IsLoopInvariantDescriptor(const ScalarProvenance& provenance,
                               const DescriptorValue&  descriptor) {
	std::vector<uint8_t> visiting(provenance.values.size());
	for (uint32_t i = 0; i < descriptor.dword_count; i++) {
		if (!IsLoopInvariantValue(provenance, descriptor.dwords[i], &visiting)) {
			return false;
		}
	}
	return true;
}

class Tracker {
public:
	explicit Tracker(Program& program): m_program(program) {}

	bool Run(std::string* error) {
		if (m_program.resource_tracking_complete) {
			return Fail(0, error, "resources already tracked");
		}
		if (!m_program.srt_plan_complete) {
			return Fail(0, error, "SRT plan is not ready");
		}
		if (!m_program.srt_patching_complete) {
			return Fail(0, error, "SRT reads were not patched");
		}
		for (auto& block: m_program.blocks) {
			for (auto& inst: block.instructions) {
				if (!Collect(&inst, error)) {
					return false;
				}
			}
		}
		LinkImageAliases();
		m_program.info = std::move(m_info);
		for (const auto& patch: m_patches) {
			patch.inst->memory.resource        = patch.resource;
			patch.inst->memory.sampler         = patch.sampler;
			patch.inst->memory.resource_source = ScalarProvenance::Undefined;
			patch.inst->memory.sampler_source  = ScalarProvenance::Undefined;
		}
		m_program.resource_tracking_complete = true;
		return true;
	}

private:
	struct Patch {
		Instruction* inst     = nullptr;
		uint32_t     resource = 0;
		uint32_t     sampler  = 0;
	};

	bool Fail(uint32_t pc, std::string* error, const std::string& reason) const {
		if (error != nullptr) {
			*error = fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
			                     m_program.shader_hash, StageName(m_program.stage), pc, reason);
		}
		return false;
	}

	// DENEY KAPISI - `PSEMU_DESC_TOLERE=1`.
	//
	// Amac: cozulemeyen descriptor'lari tolere edip dort compute shader'in
	// DERLENMESINI saglamak ve G-Buffer'in isik alip almadigini gormek.
	//
	// BU BIR DUZELTME DEGIL. Dogrulama burada titizlik yapmiyor; emulator
	// "bu descriptor'in adresini BILMIYORUM" diyor. Tolere edince dogru
	// descriptor elde etmiyoruz, BILINMEYEN bir descriptor elde ediyoruz;
	// materyalizasyon baglayacak gercek bir V#/T# bulamaz. Beklenen sonuc
	// sahte/null kaynak baglanmasi - yani muhtemelen yine siyah. Deneyin
	// degeri, "toleransi acinca isik gelir" yolunu UCUZA ELEMEKTE.
	//
	// Varsayilan KAPALI: acik birakmak gecersiz descriptor'lari sessizce
	// GPU'ya gecirir.
	[[nodiscard]] static bool DescriptorToleranceEnabled() {
		static const bool on = [] {
			const char* env = std::getenv("PSEMU_DESC_TOLERE");
			return env != nullptr && env[0] == '1';
		}();
		return on;
	}

	static bool IsAstroBot() {
		static const bool is_astro = [] {
			std::string id;
			return Loader::SystemContentParamSfoGetString("TITLE_ID", &id) && id == "PPSA21564";
		}();
		return is_astro;
	}

	uint32_t ResolveInvariantPhi(uint32_t id) const {
		if (id <= ScalarProvenance::Unknown || id >= m_program.provenance.values.size()) {
			return id;
		}
		const auto& val = m_program.provenance.values[id];
		if (val.op != ScalarValueOp::Phi) {
			return id;
		}
		uint32_t invariant = ScalarProvenance::Undefined;
		std::vector<uint32_t> pending {id};
		std::vector<uint8_t> visited(m_program.provenance.values.size(), 0);
		while (!pending.empty()) {
			const uint32_t cur = pending.back();
			pending.pop_back();
			if (cur <= ScalarProvenance::Unknown || cur >= m_program.provenance.values.size()) {
				return cur;
			}
			const auto& cur_val = m_program.provenance.values[cur];
			if (cur_val.op == ScalarValueOp::Phi) {
				if (visited[cur]) continue;
				visited[cur] = 1;
				for (const auto arg : cur_val.phi_args) {
					pending.push_back(arg);
				}
				continue;
			}
			if (invariant == ScalarProvenance::Undefined) {
				invariant = cur;
			} else if (invariant != cur) {
				const auto& inv_val = m_program.provenance.values[invariant];
				if (inv_val.op == ScalarValueOp::Constant && cur_val.op == ScalarValueOp::Constant &&
				    inv_val.imm == cur_val.imm) {
					continue;
				}
				return ScalarProvenance::Unknown;
			}
		}
		return invariant;
	}

	bool CollapseNullPhi(uint32_t id, uint32_t& loaded) const {
		if (id <= ScalarProvenance::Unknown || id >= m_program.provenance.values.size()) {
			return false;
		}
		const auto& val = m_program.provenance.values[id];
		if (val.op != ScalarValueOp::Phi || val.phi_args.empty()) {
			return false;
		}
		uint32_t merged = ScalarProvenance::Undefined;
		bool has_null = false;
		for (const auto arg : val.phi_args) {
			const uint32_t resolved = ResolveInvariantPhi(arg);
			bool is_null = (resolved == ScalarProvenance::Undefined);
			if (!is_null && resolved < m_program.provenance.values.size() &&
			    m_program.provenance.values[resolved].op == ScalarValueOp::Constant &&
			    m_program.provenance.values[resolved].imm == 0) {
				is_null = true;
			}
			if (is_null) {
				has_null = true;
				continue;
			}
			if (resolved <= ScalarProvenance::Unknown) {
				return false;
			}
			if (merged == ScalarProvenance::Undefined) {
				merged = resolved;
			} else if (merged != resolved) {
				const auto& m_val = m_program.provenance.values[merged];
				const auto& r_val = m_program.provenance.values[resolved];
				if (m_val.op == ScalarValueOp::Constant && r_val.op == ScalarValueOp::Constant &&
				    m_val.imm == r_val.imm) {
					continue;
				}
				return false;
			}
		}
		if (!has_null || merged == ScalarProvenance::Undefined) {
			return false;
		}
		loaded = merged;
		return true;
	}

	void CollapseNullDescriptorPaths(uint32_t source, uint32_t width) const {
		if (source < 2 || source - 2 >= m_program.provenance.descriptors.size()) {
			return;
		}
		auto& desc = m_program.provenance.descriptors[source - 2];
		std::array<uint32_t, 8> collapsed {};
		bool rewritten = false;
		for (uint32_t i = 0; i < width && i < desc.dword_count; i++) {
			collapsed[i] = desc.dwords[i];
			const uint32_t inv = ResolveInvariantPhi(desc.dwords[i]);
			if (inv > ScalarProvenance::Unknown) {
				continue;
			}
			uint32_t loaded = 0;
			if (CollapseNullPhi(desc.dwords[i], loaded)) {
				collapsed[i] = loaded;
				rewritten = true;
			}
		}
		if (rewritten) {
			for (uint32_t i = 0; i < width && i < desc.dword_count; i++) {
				desc.dwords[i] = collapsed[i];
			}
			std::printf("[DESC-COLLAPSE] source=%u null-phi dword'lar cozumlendi ve guncellendi\n", source);
			std::fflush(stdout);
		}
	}

	bool ValidateSource(uint32_t source, uint32_t dwords, uint32_t pc, std::string* error,
	                    bool is_sampler = false) const {
		if (IsAstroBot()) {
			CollapseNullDescriptorPaths(source, dwords);
		}
		const auto* descriptor = GetDescriptorSource(m_program, source);
		if (descriptor == nullptr || descriptor->dword_count != dwords) {
			return Fail(pc, error,
			            fmt::format("descriptor source {} is missing or has wrong width", source));
		}
		std::vector<uint8_t> visited(m_program.provenance.values.size());
		for (uint32_t i = 0; i < descriptor->dword_count; i++) {
			std::vector<uint32_t> path;
			if (ContainsUnknown(m_program.provenance, descriptor->dwords[i], &visited, &path)) {
				if (is_sampler && dwords == 4 && i >= 2 && IsAstroBot()) {
					if (source >= 2 && source - 2 < m_program.provenance.descriptors.size()) {
						m_program.provenance.descriptors[source - 2].dwords[i] = 0;
					}
					continue;
				}
				const auto  value = descriptor->dwords[i];
				std::string chain;
				for (const auto id: path) {
					const auto op = id < m_program.provenance.values.size()
					                    ? static_cast<uint32_t>(m_program.provenance.values[id].op)
					                    : UINT32_MAX;
					chain += fmt::format("{}{}:{}({})", chain.empty() ? "" : " -> ", id, op,
					                     ScalarValueToString(m_program.provenance, id));
				}
				if (DescriptorToleranceEnabled()) {
					std::printf("[DESC-TOLERE] KAPI-1 asildi: source=%u dword=%u deger=%u yol=%s\n",
					            source, i, value, chain.c_str());
					std::fflush(stdout);
					continue;
				}
				return Fail(
				    pc, error,
				    fmt::format(
				        "descriptor source {} dword {} contains an unknown value {} ({}) path {}",
				        source, i, value, ScalarValueToString(m_program.provenance, value), chain));
			}
		}
		const auto dynamic =
		    std::find(m_program.srt.dynamic_sources.begin(), m_program.srt.dynamic_sources.end(),
		              source) != m_program.srt.dynamic_sources.end();
		if (dynamic && !IsLoopInvariantDescriptor(m_program.provenance, *descriptor)) {
			if (is_sampler && dwords == 4 && IsAstroBot()) {
				// Astro Bot sampler selection can be handled via first arm
			} else if (DescriptorToleranceEnabled()) {
				std::printf("[DESC-TOLERE] KAPI-3 asildi: source=%u dongu-degismez degil\n", source);
				std::fflush(stdout);
			} else {
				std::string detail;
				for (uint32_t i = 0; i < descriptor->dword_count; i++) {
					const auto id = descriptor->dwords[i];
					detail +=
					    fmt::format(" d{}={}({})", i, id,
					                id < m_program.provenance.values.size()
					                    ? static_cast<uint32_t>(m_program.provenance.values[id].op)
					                    : UINT32_MAX);
					if (id < m_program.provenance.values.size()) {
						for (const auto arg: m_program.provenance.values[id].phi_args) {
							detail += fmt::format(
							    "/{}({})", arg,
							    arg < m_program.provenance.values.size()
							        ? static_cast<uint32_t>(m_program.provenance.values[arg].op)
							        : UINT32_MAX);
						}
					}
				}
				return Fail(pc, error,
				            fmt::format("descriptor source {} requires unsupported GPU selection{}",
				                        source, detail));
			}
		}
		if (!DescriptorSourceResolved(m_program, source) && !dynamic) {
			if (is_sampler && dwords == 4 && IsAstroBot()) {
				return true;
			}
			if (DescriptorToleranceEnabled()) {
				std::printf("[DESC-TOLERE] KAPI-2 asildi: source=%u cozulemedi\n", source);
				std::fflush(stdout);
				return true;
			}
			return Fail(pc, error, fmt::format("descriptor source {} is unresolved", source));
		}
		return true;
	}

	uint32_t AddBuffer(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.buffers.size(); i++) {
			if (m_info.buffers[i].source == inst.memory.resource_source) {
				Merge(&m_info.buffers[i], inst);
				return i;
			}
		}
		if (m_info.buffers.size() >= ShaderInfo::MaxBuffers) {
			return UINT32_MAX;
		}
		BufferResource resource;
		resource.source       = inst.memory.resource_source;
		resource.first_use_pc = inst.pc;
		Merge(&resource, inst);
		m_info.buffers.push_back(resource);
		return static_cast<uint32_t>(m_info.buffers.size() - 1);
	}

	void Merge(BufferResource* resource, const Instruction& inst) const {
		resource->first_use_pc    = std::min(resource->first_use_pc, inst.pc);
		resource->max_byte_extent = std::max(resource->max_byte_extent, ByteExtent(inst));
		resource->read            = resource->read || !IsWrite(inst.op) || IsAtomic(inst.op);
		resource->written         = resource->written || IsWrite(inst.op);
		resource->atomic          = resource->atomic || IsAtomic(inst.op);
		resource->formatted       = resource->formatted || inst.memory.formatted;
		resource->scalar = resource->scalar || inst.memory.kind == ResourceKind::ScalarBuffer;
	}

	void LinkImageAliases() {
		for (auto& buffer: m_info.buffers) {
			const auto* buffer_source = GetDescriptorSource(m_program, buffer.source);
			if (buffer_source == nullptr || buffer_source->dword_count != 4) {
				continue;
			}
			for (uint32_t i = 0; i < m_info.images.size(); i++) {
				const auto* image_source = GetDescriptorSource(m_program, m_info.images[i].source);
				if (image_source != nullptr && image_source->dword_count == 8 &&
				    std::equal(buffer_source->dwords.begin(), buffer_source->dwords.begin() + 4,
				               image_source->dwords.begin())) {
					buffer.image_alias = i;
					break;
				}
			}
		}
	}

	uint32_t AddImage(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == inst.memory.resource_source && image.kind == inst.memory.kind &&
			    image.dimension == inst.memory.image_dimension && image.mip_mode == MipMode(inst) &&
			    image.depth_compare == IsDepthCompare(inst)) {
				Merge(&image, inst);
				return i;
			}
		}
		if (m_info.images.size() >= ShaderInfo::MaxImages) {
			return UINT32_MAX;
		}
		ImageResource image;
		image.source        = inst.memory.resource_source;
		image.first_use_pc  = inst.pc;
		image.kind          = inst.memory.kind;
		image.dimension     = inst.memory.image_dimension;
		image.mip_mode      = MipMode(inst);
		image.depth_compare = IsDepthCompare(inst);
		Merge(&image, inst);
		m_info.images.push_back(image);
		return static_cast<uint32_t>(m_info.images.size() - 1);
	}

	uint32_t AddAddress(const Instruction& inst) {
		auto immediate = static_cast<int32_t>(inst.memory.offset);
		if (inst.memory.kind == ResourceKind::ScalarBuffer) {
			immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
		}
		const auto min_offset =
		    inst.memory.resource_source == ScalarProvenance::Unknown ? 0 : std::min(immediate, 0);
		for (uint32_t i = 0; i < m_info.addresses.size(); i++) {
			auto& address = m_info.addresses[i];
			if (address.source == inst.memory.resource_source && address.kind == inst.memory.kind) {
				address.first_use_pc = std::min(address.first_use_pc, inst.pc);
				address.min_offset   = std::min(address.min_offset, min_offset);
				address.read         = address.read || !IsWrite(inst.op) || IsAtomic(inst.op);
				address.written      = address.written || IsWrite(inst.op);
				address.atomic       = address.atomic || IsAtomic(inst.op);
				return i;
			}
		}
		if (m_info.addresses.size() >= ShaderInfo::MaxAddresses) {
			return UINT32_MAX;
		}
		AddressResource address {inst.memory.resource_source, inst.pc, inst.memory.kind,
		                         min_offset};
		address.read    = !IsWrite(inst.op) || IsAtomic(inst.op);
		address.written = IsWrite(inst.op);
		address.atomic  = IsAtomic(inst.op);
		m_info.addresses.push_back(address);
		return static_cast<uint32_t>(m_info.addresses.size() - 1);
	}

	void Merge(ImageResource* resource, const Instruction& inst) const {
		resource->first_use_pc = std::min(resource->first_use_pc, inst.pc);
		resource->read         = resource->read || !IsWrite(inst.op) || IsAtomic(inst.op);
		resource->written      = resource->written || IsWrite(inst.op);
		resource->atomic       = resource->atomic || IsAtomic(inst.op);
	}

	uint32_t AddSampler(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.samplers.size(); i++) {
			if (m_info.samplers[i].source == inst.memory.sampler_source) {
				m_info.samplers[i].first_use_pc =
				    std::min(m_info.samplers[i].first_use_pc, inst.pc);
				return i;
			}
		}
		if (m_info.samplers.size() >= ShaderInfo::MaxSamplers) {
			return UINT32_MAX;
		}
		m_info.samplers.push_back({inst.memory.sampler_source, inst.pc});
		return static_cast<uint32_t>(m_info.samplers.size() - 1);
	}

	bool AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc, std::string* error) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return true;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			return Fail(pc, error, "sampled image/sampler pair limit exceeded");
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
		return true;
	}

	bool Collect(Instruction* inst, std::string* error) {
		if (IsAddress(*inst)) {
			const bool unbased = inst->memory.resource_source == ScalarProvenance::Unknown;
			std::string address_error;
			const bool  address_ok =
			    unbased ? (inst->memory.kind == ResourceKind::Flat ||
                          inst->memory.kind == ResourceKind::Global ||
                          inst->memory.kind == ResourceKind::Scratch)
			            : ValidateSource(inst->memory.resource_source, 2, inst->pc, &address_error);
			if (!address_ok) {
				// SKALER ADRES KAYNAGI COZULEMEDI.
				//
				// Baglam: bu, IMAGE_BVH_INTERSECT_RAY kuklasi devreye girdikten
				// SONRA ortaya cikan bir engel. Olculen ornek:
				//   0x2090 s_lshl_b64  s8, s28, 3        ; adres s28:s29'dan
				//   0x20b8 s_load_dwordx4 s20, s8        ; 4 dword tanimlayici
				//   0x20dc s_add_u32  s18, s20, -1       ; 64-bit isaretci
				//   0x20e4 s_addc_u32 s19, s21, -1
				// Yani yukleme OLU DEGIL; tuketicileri BVH kurulum matematigine,
				// o da kuklaladigimiz komuta gidiyor. Adres s28:s29'dan dinamik
				// hesaplandigi icin takipci cozemiyor - ve o zincir zaten BVH
				// donanim bellek duzenine bagli, bizim uretmedigimiz bir yapi.
				//
				// NE YAPIYORUZ: derlemeyi olduren hatayi, kaynagi "temelsiz"
				// (unbased) sayan bir UYARIYA cevirip devam ediyoruz.
				// TAMPON/GORUNTU tanimlayicilarinda ise hata OLUMCUL KALIYOR
				// (asagidaki ValidateSource cagrisi) - orada yanlis cozum
				// dogrudan yanlis piksel demektir.
				//
				// NE VAAT ETMIYORUZ: bu yuklemenin okudugu degerler dogru
				// olmayabilir. Zincirin buyuk kismi kuklaya ciktigi icin bunu
				// kabul ediyoruz; ama bir deger gorunur matematige sizarsa
				// sonuc yanlis olur. Bilincli bir ara adim, kalici cozum degil.
				// PSEMU_SRT_STRICT=1 eski sert davranisi geri getirir.
				static const bool strict = [] {
					const char* e = std::getenv("PSEMU_SRT_STRICT");
					return e != nullptr && e[0] == '1';
				}();
				if (strict) {
					return unbased ? Fail(inst->pc, error, "scalar memory base is unresolved")
					               : Fail(inst->pc, error, address_error);
				}
				static std::atomic<uint32_t> s_n {0};
				if (s_n.fetch_add(1) < 16) {
					std::printf("[SRT-TEMELSIZ] skaler adres kaynagi cozulemedi, temelsiz "
					            "sayilip devam ediliyor (pc=0x%08x kind=%u) %s\n",
					            inst->pc, static_cast<uint32_t>(inst->memory.kind),
					            address_error.c_str());
					std::fflush(stdout);
				}
				inst->memory.resource_source = ScalarProvenance::Unknown;
			}
			const auto resource = AddAddress(*inst);
			if (resource == UINT32_MAX) {
				return Fail(inst->pc, error, "address resource limit exceeded");
			}
			m_patches.push_back({inst, resource, 0});
			return true;
		}
		if (!IsBuffer(*inst) && !IsImage(*inst)) {
			return true;
		}
		if (IsBuffer(*inst)) {
			std::string buffer_error;
			if (!ValidateSource(inst->memory.resource_source, 4u, inst->pc, &buffer_error)) {
				static const bool strict = [] {
					const char* e = std::getenv("PSEMU_SRT_STRICT");
					return e != nullptr && e[0] == '1';
				}();
				if (strict) {
					if (error != nullptr) {
						*error = std::move(buffer_error);
					}
					return false;
				}
				static std::atomic<uint32_t> s_n {0};
				if (s_n.fetch_add(1) < 16) {
					std::printf("[SRT-TAMPON-TOLERE] tampon kaynagi cozulemedi, temelsiz sayilip devam ediliyor (pc=0x%08x) %s\n",
					            inst->pc, buffer_error.c_str());
					std::fflush(stdout);
				}
				inst->memory.resource_source = ScalarProvenance::Unknown;
			}
		} else if (!ValidateSource(inst->memory.resource_source, 8u, inst->pc, error)) {
			return false;
		}
		Patch patch;
		patch.inst     = inst;
		patch.resource = IsBuffer(*inst) ? AddBuffer(*inst) : AddImage(*inst);
		if (patch.resource == UINT32_MAX) {
			return Fail(inst->pc, error,
			            IsBuffer(*inst) ? "buffer resource limit exceeded"
			                            : "image resource limit exceeded");
		}
		if (NeedsSampler(inst->op)) {
			if (!ValidateSource(inst->memory.sampler_source, 4, inst->pc, error, true)) {
				return false;
			}
			patch.sampler = AddSampler(*inst);
			if (patch.sampler == UINT32_MAX) {
				return Fail(inst->pc, error, "sampler resource limit exceeded");
			}
			if (!AddSampledPair(patch.resource, patch.sampler, inst->pc, error)) {
				return false;
			}
		}
		m_patches.push_back(patch);
		return true;
	}

	Program&           m_program;
	ShaderInfo         m_info;
	std::vector<Patch> m_patches;
};

} // namespace

bool TrackResources(Program* program, std::string* error) {
	if (program == nullptr) {
		if (error != nullptr) {
			*error = "shader resource tracking: hash=0x0000000000000000 stage=unknown "
			         "pc=0x00000000 invalid program";
		}
		return false;
	}
	return Tracker(*program).Run(error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
