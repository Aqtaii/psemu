#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/shader.h"
#include "common/assert.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <spirv/unified1/spirv.hpp>
using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;
namespace Common {
int DbgExitHandler(char const*,int line,std::string_view text){std::fprintf(stderr,"fatal %d: %.*s\n",line,int(text.size()),text.data());std::abort();}
int DbgExitHandler(char const*,int,fmt::text_style,std::string_view){std::abort();}
int DbgExitIfHandler(char const*,char const*,int){std::abort();}
int DbgNotImplementedHandler(char const*,char const*,int){std::abort();}
void DbgExit(int){std::abort();}
}
static IR::Operand Imm(uint32_t value){IR::Operand a;a.kind=IR::OperandKind::ImmediateU32;a.imm=value;return a;}
static IR::Operand V(uint32_t index){IR::Operand a;a.kind=IR::OperandKind::Register;a.reg={IR::RegisterFile::Vector,index};return a;}
int main(){
 IR::Program p;p.stage=ShaderType::Compute;p.wave_size=32;p.user_data_count=0;
 p.srt_plan_complete=true;p.srt_patching_complete=true;p.resource_tracking_complete=true;p.shader_info_complete=true;p.binding_layout_complete=true;
 p.blocks.resize(1);p.blocks[0].terminator.kind=CFG::TerminatorKind::Return;
 for(uint32_t i:{1u,2u}){IR::Instruction move;move.op=IR::Opcode::MoveU32;move.execution=IR::InstructionExecution::Vector;move.dst=V(i);move.src_count=1;move.src[0]=Imm(0);p.blocks[0].instructions.push_back(move);}
 IR::ImageResource image;image.kind=IR::ResourceKind::StorageImageUint;image.dimension=Decoder::ImageDimension::Dim2D;
 image.read=image.written=image.atomic=true;image.storage_swizzle=0x204;p.info.images.push_back(image);
 IR::DescriptorBinding binding;binding.kind=IR::DescriptorBindingKind::StorageUint2D;binding.binding=0;binding.resources={0};p.bindings.descriptors.push_back(binding);
 IR::Instruction atomic;atomic.op=IR::Opcode::AtomicAddU32;atomic.execution=IR::InstructionExecution::Vector;atomic.dst=V(0);atomic.src_count=2;atomic.src[0]=Imm(1);atomic.src[1]=V(1);
 atomic.memory.kind=IR::ResourceKind::StorageImageUint;atomic.memory.resource=0;atomic.memory.image_dimension=Decoder::ImageDimension::Dim2D;
 atomic.memory.image_address_components=2;atomic.memory.data_bits=32;atomic.memory.data_dwords=1;atomic.memory.component_count=1;atomic.memory.glc=true;
 p.blocks[0].instructions.push_back(atomic);
 IR::ResourceSnapshot snapshot;IR::DescriptorValue descriptor;descriptor.dword_count=8;
 descriptor.dwords={0x100u,0x01400000u,0u,0x90000204u,0u,0x00700000u,0u,0u};snapshot.images.push_back(descriptor);
 ShaderComputeInputInfo compute;compute.threads_num[0]=compute.threads_num[1]=8;compute.threads_num[2]=1;compute.wave_size=32;
 std::vector<uint32_t> code;std::string error;
 if(!Spirv::EmitProgram(p,snapshot,nullptr,nullptr,&compute,&code,&error)){std::fprintf(stderr,"atomic emit: %s\n",error.c_str());return 1;}
 std::unordered_map<uint32_t,uint32_t> constants;
 bool atomic_found=false,image_barrier=false;
 for(size_t i=5;i<code.size();) {
  const auto count=code[i]>>16,op=code[i]&65535u;
  if(!count || count>code.size()-i)return 1;
  if(op==spv::OpConstant && count==4)constants[code[i+2]]=code[i+3];
  if(op==spv::OpAtomicIAdd)atomic_found=true;
  if(op==spv::OpMemoryBarrier && count==3) {
   const auto it=constants.find(code[i+2]);
   const uint32_t required=spv::MemorySemanticsAcquireReleaseMask|spv::MemorySemanticsImageMemoryMask;
   image_barrier|=it!=constants.end() && it->second==required;
  }
  i+=count;
 }
 if(!atomic_found || !image_barrier){std::fprintf(stderr,"Missing image atomic/barrier in production SPIR-V\n");return 1;}
 std::ofstream out("atomic_image_emitter.spv",std::ios::binary);out.write(reinterpret_cast<const char*>(code.data()),code.size()*4);if(!out)return 1;
 std::printf("[ATOMIC-IMAGE-EMITTER] production SPIR-V words=%zu ImageMemory_barrier=checked synthetic_IR=1\n",code.size());
}

