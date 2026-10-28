#include "graphics/shader/recompiler/ScalarAluOps.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/MergedMeshResources.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"
#include "common/assert.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <source_location>
using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;
namespace Common {
int DbgExitHandler(char const*,int line,std::string_view text){std::fprintf(stderr,"fatal %d: %.*s\n",line,int(text.size()),text.data());std::abort();}
int DbgExitHandler(char const*,int,fmt::text_style,std::string_view){std::abort();}
int DbgExitIfHandler(char const*,char const*,int){std::abort();}
int DbgNotImplementedHandler(char const*,char const*,int){std::abort();}
void DbgExit(int){std::abort();}
}
void Require(bool ok,const std::string& error={},std::source_location where=std::source_location::current()){
 if(!ok){std::fprintf(stderr,"scalar U64 regression line %u: %s\n",where.line(),error.c_str());std::exit(1);}
}
int main(int argc,char** argv){
 std::string error;
 const std::array<uint32_t,1> raw{0xbf128002};Decoder::Instruction captured;
 Require(Decoder::DecodeSopc(0x3bc,raw,0,&captured,&error),error);
 Require(captured.opcode==Decoder::Opcode::SCmpEqU64 && captured.pc==0x3bc && captured.word==raw[0]);
 Require(captured.dst.kind==Decoder::OperandKind::Scc && captured.src_count==2);
 Require(captured.src0.kind==Decoder::OperandKind::Sgpr && captured.src0.reg==2);
 Require(captured.src1.kind==Decoder::OperandKind::IntegerInlineConstant && captured.src1.value==0);
 Require(Decoder::OpcodeToString(captured.opcode)=="s_cmp_eq_u64");
 // Pair collection must include upper words even if no other instruction uses them.
 IR::Program pair;pair.blocks.resize(1);IR::Instruction eq;eq.op=IR::Opcode::CompareEqU64;
 eq.dst.kind=IR::OperandKind::Register;eq.dst.reg={IR::RegisterFile::Scc,0};eq.src_count=2;
 eq.src[0].kind=eq.src[1].kind=IR::OperandKind::Register;
 eq.src[0].reg={IR::RegisterFile::Scalar,2};eq.src[1].reg={IR::RegisterFile::Scalar,6};
 pair.blocks[0].instructions.push_back(eq);std::vector<Spirv::Emitter::RegisterBinding> regs;
 Spirv::Emitter::CollectRegisters(pair,&regs);
 for(uint32_t index:{2u,3u,6u,7u}){bool found=false;for(auto& r:regs)found|=r.reg.file==IR::RegisterFile::Scalar && r.reg.index==index;Require(found);}
 // A standalone pair compare must seed both source dwords from user data.
 for(uint32_t opcode:{0xbf120602u,0xbf130602u}) for(bool overwrite_high:{false,true}){
  std::vector<uint32_t> code; if(overwrite_high) code.push_back(0xbe830381u);
  code.insert(code.end(),{opcode,0xbf840001,0xbf800000,0xbf810000});
  Decoder::Program decoded;Require(Decoder::DecodeProgram(code,&decoded,&error),error);
  CFG::Graph graph;Require(CFG::BuildGraph(decoded,&graph,&error),error);Require(CFG::StructurizeForLowering(&graph,&error),error);
  IR::Program program;Require(IR::LowerProgram(decoded,graph,ShaderType::Compute,32,&program,&error),error);
  program.user_data_count=8;
  Require(IR::BuildScalarProvenance(&program,&error),error);Require(IR::BuildSrtPlan(&program,&error),error);
  Require(IR::PatchSrtReads(&program,&error),error);Require(IR::TrackResources(&program,&error),error);
  const std::array<uint32_t,8> user_data{0,0,0,1,0,0,0,0};
  IR::SrtRuntime runtime;runtime.user_data=user_data;IR::ResourceSnapshot resources;Require(IR::MaterializeResources(program,runtime,&resources,&error),error);
  Require(IR::SpecializeResources(&program,resources,&error),error);
  ShaderComputeInputInfo info{};info.threads_num[0]=32;info.threads_num[1]=info.threads_num[2]=1;info.wave_size=32;
  IR::ShaderInfoOptions opts;opts.compute=&info;Require(IR::CollectShaderInfo(&program,opts,&error),error);
  Require(IR::AllocateBindings(&program,{},&error),error);
  Require(program.bindings.user_data_registers==(overwrite_high ? std::vector<uint32_t>({2,6,7}) : std::vector<uint32_t>({2,3,6,7})));
  std::vector<uint32_t> words;Require(Spirv::EmitProgram(program,resources,nullptr,nullptr,&info,&words,&error),error);
  const auto filename=std::string(opcode==0xbf120602u ? "scalar_eq_userdata" : "scalar_lg_userdata")+(overwrite_high ? "_overwrite.spv" : ".spv");
  std::ofstream out(filename,std::ios::binary);out.write(reinterpret_cast<const char*>(words.data()),words.size()*4);Require(bool(out));
 }
 for(bool equal:{true,false}){
  const std::array<uint32_t,6> code{0xbe820380,0xbe830381,equal?0xbf128002u:0xbf138002u,0xbf840001,0xbf800000,0xbf810000};
  Decoder::Program decoded;Require(Decoder::DecodeProgram(code,&decoded,&error),error);
  CFG::Graph graph;Require(CFG::BuildGraph(decoded,&graph,&error),error);Require(CFG::StructurizeForLowering(&graph,&error),error);
  IR::Program program;Require(IR::LowerProgram(decoded,graph,ShaderType::Compute,32,&program,&error),error);
  unsigned comparisons=0;
  for(auto& block:program.blocks)for(auto& inst:block.instructions)if(inst.op==(equal?IR::Opcode::CompareEqU64:IR::Opcode::CompareNeU64)){
   comparisons++;Require(inst.execution==IR::InstructionExecution::Scalar && inst.dst.reg.file==IR::RegisterFile::Scc);
  }
  Require(comparisons==1);program.user_data_count=0;
  Require(IR::BuildScalarProvenance(&program,&error),error);Require(IR::BuildSrtPlan(&program,&error),error);
  Require(IR::PatchSrtReads(&program,&error),error);Require(IR::TrackResources(&program,&error),error);
  IR::SrtRuntime runtime;IR::ResourceSnapshot resources;Require(IR::MaterializeResources(program,runtime,&resources,&error),error);
  Require(IR::SpecializeResources(&program,resources,&error),error);
  ShaderComputeInputInfo info{};info.threads_num[0]=32;info.threads_num[1]=info.threads_num[2]=1;info.wave_size=32;
  IR::ShaderInfoOptions opts;opts.compute=&info;Require(IR::CollectShaderInfo(&program,opts,&error),error);
  Require(IR::AllocateBindings(&program,{},&error),error);
  std::vector<uint32_t> words;Require(Spirv::EmitProgram(program,resources,nullptr,nullptr,&info,&words,&error),error);
  std::ofstream out(equal?"scalar_eq_u64.spv":"scalar_lg_u64.spv",std::ios::binary);out.write(reinterpret_cast<const char*>(words.data()),words.size()*4);Require(bool(out));
 }
 if(argc==2){
  std::ifstream in(argv[1],std::ios::binary|std::ios::ate);Require(bool(in));const auto bytes=in.tellg();Require(bytes>0 && bytes<128*1024 && bytes%4==0);
  std::vector<uint32_t> words(size_t(bytes)/4);in.seekg(0);in.read(reinterpret_cast<char*>(words.data()),bytes);Require(bool(in));
  Decoder::Program decoded;Require(Decoder::DecodeProgram(words,&decoded,&error),error);
  CFG::Graph graph;Require(CFG::BuildGraph(decoded,&graph,&error),error);
  IR::Program program;Require(IR::LowerProgram(decoded,graph,ShaderType::Compute,32,&program,&error),error);
  unsigned count=0;for(auto& block:program.blocks)for(auto& inst:block.instructions)count+=inst.op==IR::Opcode::CompareEqU64;
  Require(count>0);std::ofstream dump("captured_scalar_u64.rdna2");dump<<Decoder::ProgramToString(decoded);
  std::printf("[SCALAR-U64-CAPTURE] instructions=%zu blocks=%zu EqU64=%u decode_CFG_IR=passed resources=not-materialized GPU=not-executed\n",decoded.instructions.size(),program.blocks.size(),count);
 }
 std::puts("PASS: captured SOPC0x12, 64-bit source pairs, scalar SCC lowering and real Compute SPIR-V emission");
}
