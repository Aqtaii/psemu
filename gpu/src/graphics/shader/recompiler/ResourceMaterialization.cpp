#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ScalarProvenance.h"

#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

Decoder::ImageDimension DescriptorDimension(const DescriptorValue& descriptor, ResourceKind kind) {
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube:
		case Prospero::ImageType::kColor1DArray:
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaaArray: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor1D:
			return kind == ResourceKind::StorageImage || kind == ResourceKind::StorageImageUint
			           ? Decoder::ImageDimension::Dim1D : Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2D:
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2D;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] != 0 || descriptor.dwords[1] != 0 || ((descriptor.dwords[3] >> 28u) & 0x8u) != 0;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

uint64_t AddressSpecialization(const AddressResource&           resource,
                               const ResourceSnapshot::Address& snapshot) {
	return resource.kind == ResourceKind::Flat || resource.source == ScalarProvenance::Unknown
	           ? snapshot.binding_base
	           : snapshot.guest_base - snapshot.binding_base;
}

} // namespace

bool BuildPushConstantPayload(const Program& program, const ResourceSnapshot& snapshot,
                              const MeshPointGroupPayload* point, std::vector<uint32_t>* output,
                              std::string* error) {
 auto fail=[&](const char* message){if(error) *error=message;return false;};
 if(output==nullptr || !program.binding_layout_complete) return fail("push payload layout is not ready");
 const auto& layout=program.bindings;
 const bool has_point=layout.mesh_point_input_dword!=BindingLayout::NoMeshPointInputs;
 if(has_point!=(point!=nullptr) || (has_point && (program.stage!=ShaderType::Mesh || point->vertex_count>64)))
  return fail("invalid or missing Mesh point draw payload");
 if(layout.push_constant_offset%4 || layout.push_constant_size%4 ||
    uint64_t(layout.push_constant_offset)+layout.push_constant_size>128) return fail("invalid push payload range");
 std::vector<uint32_t> words;
 if(FindBinding(layout,DescriptorBindingKind::UserData)==nullptr) {
  for(const auto reg:layout.user_data_registers) {
   if(reg<program.user_data_base || reg-program.user_data_base>=snapshot.user_data.size())
    return fail("push payload is missing ES user data");
   words.push_back(snapshot.user_data[reg-program.user_data_base]);
  }
 }
 if(has_point) {
  if(layout.mesh_point_input_dword!=words.size()) return fail("point draw payload overlaps ES data");
  words.insert(words.end(),{point->vertex_count,point->first_vertex,point->instance,
                           point->gs_user_data_low,point->gs_user_data_high});
 }
 if(words.size()*4!=layout.push_constant_size) return fail("push payload does not match layout");
 *output=std::move(words);
 return true;
}

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot,
                              std::string* error) {
	if (!program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "shader resources were not tracked";
		}
		return false;
	}
    if(!ValidateGuestReadFootprint(snapshot.guest_reads) ||
       (!program.uses_guest_address_table && !snapshot.guest_reads.empty())) {
        if(error)*error="invalid guest source read footprint in resource snapshot";
        return false;
    }
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
	    snapshot.samplers.size() != program.info.samplers.size() ||
	    snapshot.addresses.size() != program.info.addresses.size()) {
		if (error != nullptr) {
			*error = "resource snapshot does not match dense shader topology";
		}
		return false;
	}
	if (snapshot.flattened_srt.size() != program.srt.reads.size()) {
		if (error != nullptr) {
			*error = "flattened SRT snapshot does not match the shader plan";
		}
		return false;
	}
	if (program.binding_layout_complete) {
		for (const auto reg: program.bindings.user_data_registers) {
			if (reg < program.user_data_base ||
			    reg - program.user_data_base >= snapshot.user_data.size()) {
				if (error != nullptr) {
					*error = fmt::format("runtime snapshot is missing user SGPR {}", reg);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto alias = program.info.buffers[i].image_alias;
		if (alias != BufferResource::NoImageAlias && alias >= program.info.images.size()) {
			if (error != nullptr) {
				*error = fmt::format("buffer resource {} has invalid image alias {}", i, alias);
			}
			return false;
		}
	}
	const auto CheckWidth = [&](const auto& values, uint32_t width, const char* kind) {
		for (uint32_t i = 0; i < values.size(); i++) {
			if (values[i].dword_count != width) {
				if (error != nullptr) {
					*error = fmt::format("{} descriptor {} has {} dwords", kind, i,
					                     values[i].dword_count);
				}
				return false;
			}
		}
		return true;
	};
	for (uint32_t i = 0; i < snapshot.addresses.size(); i++) {
		if (snapshot.addresses[i].binding_base > snapshot.addresses[i].guest_base) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} binds above its guest base", i);
			}
			return false;
		}
	}
	return CheckWidth(snapshot.buffers, 4, "buffer") && CheckWidth(snapshot.images, 8, "image") &&
	       CheckWidth(snapshot.samplers, 4, "sampler");
}

bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot,
                                    std::string* error) {
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
		return false;
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto&          buffer = program.info.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		if (buffer.packed_stride != descriptor.PackedStride() ||
		    buffer.descriptor_format != descriptor.Format()) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} no longer matches specialization", i);
			}
			return false;
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image      = program.info.images[i];
		const auto& descriptor = snapshot.images[i];
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor, image.kind);
		if (dimension == Decoder::ImageDimension::Unknown || dimension != image.dimension) {
			if (error != nullptr) {
				*error =
				    fmt::format("image descriptor {} no longer matches specialized dimension", i);
			}
			return false;
		}
		if (image.kind == ResourceKind::Image || image.kind == ResourceKind::ImageUint ||
		    image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			const bool storage = image.kind == ResourceKind::StorageImage ||
			                     image.kind == ResourceKind::StorageImageUint;
			if (storage && image.storage_swizzle != DescriptorImageSwizzle(descriptor)) {
				if (error != nullptr) {
					*error = fmt::format("storage image descriptor {} changed swizzle", i);
				}
				return false;
			}
			const auto uint_descriptor =
			    Prospero::IsUintTextureFormat((descriptor.dwords[1] >> 20u) & 0x1ffu);
			const auto uint_program = image.kind == ResourceKind::ImageUint ||
			                          image.kind == ResourceKind::StorageImageUint;
			if (uint_descriptor != uint_program && !(image.atomic && uint_program)) {
				if (error != nullptr) {
					*error =
					    fmt::format("image descriptor {} no longer matches specialized format", i);
				}
				return false;
			}
			if (image.depth_compare && uint_program) {
				if (error != nullptr) {
					*error = fmt::format("integer image descriptor {} uses depth comparison", i);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		if (program.info.addresses[i].specialized_base !=
		    AddressSpecialization(program.info.addresses[i], snapshot.addresses[i])) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} no longer matches specialization", i);
			}
			return false;
		}
	}
	return true;
}

bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot* snapshot, std::string* error) {
	if (snapshot == nullptr || !program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = snapshot == nullptr ? "invalid resource snapshot"
			                             : "shader resources were not tracked";
		}
		return false;
	}

	std::vector<DescriptorSourceRequest> requests;
	requests.reserve(program.info.buffers.size() + program.info.images.size() +
	                 program.info.samplers.size() + program.info.addresses.size());
	for (const auto& buffer: program.info.buffers) {
		requests.push_back({buffer.source, buffer.first_use_pc});
	}
	for (const auto& image: program.info.images) {
		requests.push_back({image.source, image.first_use_pc});
	}
	for (const auto& sampler: program.info.samplers) {
		requests.push_back({sampler.source, sampler.first_use_pc});
	}
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			requests.push_back({address.source, address.first_use_pc});
		}
	}

	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
    struct ReadTrace {
        const SrtRuntime* source;
        std::vector<uint64_t> addresses;
    } trace{&runtime,{}};
    auto evaluated_runtime=runtime;
    if(program.uses_guest_address_table) {
        evaluated_runtime.userdata=&trace;
        evaluated_runtime.read_memory=[](void* userdata,uint64_t address,uint32_t* value) {
            auto& trace=*static_cast<ReadTrace*>(userdata);
            if(trace.addresses.size()>=MaxGuestReadDwords ||
               !ReadSrtRuntimeMemory(*trace.source,address,value))return false;
            trace.addresses.push_back(address);
            return true;
        };
    }
	if (!EvaluateRuntimeSources(program, requests, evaluated_runtime, &values, &flattened_srt, error)) {
		return false;
	}

    // A checked Compute S_LOAD may have no host descriptor consumer at all.
    // Prepare only exact entry-block reads whose two reaching definitions are
    // unchanged initial user SGPRs. No guest reads, pointer chasing, conditional
    // blocks, dynamic offsets, or replacement of GPU instructions are involved.
    if(program.uses_guest_address_table && program.stage==ShaderType::Compute &&
       !program.blocks.empty() && program.blocks.front().start_pc==0 &&
       program.blocks.front().predecessors.empty()) {
        for(const auto& inst:program.blocks.front().instructions) {
            const auto& group=inst.scalar_memory_group;
            if(inst.op!=Opcode::SLoadDword || !inst.memory.guest_address_read ||
               !group.valid || group.descriptor_dwords!=2 ||
               group.dynamic_offset.kind!=OperandKind::ImmediateU32)continue;
            const auto* source=GetDescriptorSource(program,inst.memory.resource_source);
            if(source==nullptr || source->dword_count!=2)continue;
            uint32_t words[2]{};bool initial=true;
            for(uint32_t i=0;i<2;++i) {
                const auto id=source->dwords[i];
                if(id<=ScalarProvenance::Unknown || id>=program.provenance.values.size()){initial=false;break;}
                const auto& value=program.provenance.values[id];
                if(value.op!=ScalarValueOp::UserData || value.imm<program.user_data_base ||
                   value.imm-program.user_data_base>=runtime.user_data.size()){initial=false;break;}
                words[i]=runtime.user_data[value.imm-program.user_data_base];
            }
            if(!initial || words[1]>=65536u)continue;
            const auto base=uint64_t(words[0])|(uint64_t(words[1])<<32u);
            const auto offset=uint64_t(group.dynamic_offset.imm&~3u)+uint64_t(inst.memory.offset&~3u);
            const auto address=base+offset;
            // Invalid GPU addresses remain invalid; do not wrap them into resident memory.
            if(!address || (address&3u) || address>0x0000fffffffffffcull)continue;
            if(trace.addresses.size()>=MaxGuestReadDwords) {
                if(error)*error="excessive Compute entry scalar read footprint";
                return false;
            }
            trace.addresses.push_back(address);
        }
    }
	ResourceSnapshot next;
    if(!BuildGuestReadFootprint(trace.addresses,&next.guest_reads)) {
        if(error)*error="invalid or excessive guest source read footprint";
        return false;
    }
	auto             cursor = values.begin();
	next.buffers.assign(cursor, cursor + program.info.buffers.size());
	cursor += program.info.buffers.size();
	next.images.assign(cursor, cursor + program.info.images.size());
	cursor += program.info.images.size();
	for (auto& descriptor: next.images) {
		if (!ValidImageDescriptor(descriptor)) {
			descriptor.dwords.fill(0);
		}
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	cursor += program.info.samplers.size();
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			const auto value = *cursor++;
			auto       base  = (static_cast<uint64_t>(value.dwords[0]) |
			                    static_cast<uint64_t>(value.dwords[1]) << 32u) &
			                   AddressMask;
			if (address.kind == ResourceKind::ScalarBuffer) {
				base &= ~uint64_t {3};
			}
			const auto before = static_cast<uint64_t>(-static_cast<int64_t>(address.min_offset));
			const auto binding_base = address.kind == ResourceKind::Flat
			                              ? base & ~(FlatAddressWindowSize - 1u)
			                          : base >= before ? base - before
			                                           : 0;
			next.addresses.push_back({base, binding_base});
		} else {
			if (!runtime.flat_memory_base.has_value()) {
				if (error != nullptr) {
					*error =
					    fmt::format("unbased {} address at pc 0x{:08x} requires runtime "
					                "guest-address translation",
					                address.kind == ResourceKind::Flat ? "FLAT" : "global/scratch",
					                address.first_use_pc);
				}
				return false;
			}
			next.addresses.push_back({*runtime.flat_memory_base, *runtime.flat_memory_base});
		}
	}
	next.flattened_srt = std::move(flattened_srt);
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	if (!ValidateResourceSnapshot(program, next, error)) {
		return false;
	}
	*snapshot = std::move(next);
	return true;
}

bool SpecializeResources(Program* program, const ResourceSnapshot& snapshot, std::string* error) {
	if (program == nullptr || !program->resource_tracking_complete ||
	    program->shader_info_complete || program->binding_layout_complete) {
		if (error != nullptr) {
			*error = program == nullptr ? "invalid resource specialization program"
			         : !program->resource_tracking_complete ? "shader resources were not tracked"
			                                                : "resource specialization is too late";
		}
		return false;
	}
	if (!ValidateResourceSnapshot(*program, snapshot, error)) {
		return false;
	}

	auto next = program->info;
	for (uint32_t i = 0; i < next.buffers.size(); i++) {
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		next.buffers[i].packed_stride     = descriptor.PackedStride();
		next.buffers[i].descriptor_format = descriptor.Format();
	}
	for (uint32_t i = 0; i < next.addresses.size(); i++) {
		next.addresses[i].specialized_base =
		    AddressSpecialization(next.addresses[i], snapshot.addresses[i]);
	}
	for (uint32_t i = 0; i < next.images.size(); i++) {
		const auto& descriptor = snapshot.images[i];
		auto&       image      = next.images[i];
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor, image.kind);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			if (error != nullptr) {
				*error = fmt::format(
				    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
				    "{:08x},{:08x},{:08x},{:08x}",
				    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0],
				    descriptor.dwords[1], descriptor.dwords[2], descriptor.dwords[3],
				    descriptor.dwords[4], descriptor.dwords[5], descriptor.dwords[6],
				    descriptor.dwords[7]);
			}
			return false;
		}
		image.dimension = descriptor_dimension;
		if (image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			image.storage_swizzle = DescriptorImageSwizzle(descriptor);
		}
		if (Prospero::IsUintTextureFormat((descriptor.dwords[1] >> 20u) & 0x1ffu)) {
			switch (image.kind) {
				case ResourceKind::Image: image.kind = ResourceKind::ImageUint; break;
				case ResourceKind::StorageImage: image.kind = ResourceKind::StorageImageUint; break;
				default: break;
			}
		}
	}
	struct ImagePatch {
		Instruction*            inst;
		ResourceKind            kind;
		Decoder::ImageDimension dimension;
	};
	std::vector<ImagePatch> patches;
	for (auto& block: program->blocks) {
		for (auto& inst: block.instructions) {
			if (inst.memory.kind != ResourceKind::Image &&
			    inst.memory.kind != ResourceKind::ImageUint &&
			    inst.memory.kind != ResourceKind::StorageImage &&
			    inst.memory.kind != ResourceKind::StorageImageUint) {
				continue;
			}
			if (inst.memory.resource >= next.images.size()) {
				if (error != nullptr) {
					*error = fmt::format("image instruction at pc 0x{:08x} has invalid resource {}",
					                     inst.pc, inst.memory.resource);
				}
				return false;
			}
			const auto& image = next.images[inst.memory.resource];
			patches.push_back({&inst, image.kind, image.dimension});
		}
	}
	program->info = std::move(next);
	for (const auto& patch: patches) {
		patch.inst->memory.kind            = patch.kind;
		patch.inst->memory.image_dimension = patch.dimension;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
