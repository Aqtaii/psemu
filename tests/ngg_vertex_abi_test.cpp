#include "graphics/shader/recompiler/ShaderIR.h"
#include <stdexcept>
#include <cstdio>
namespace IR=Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;
struct EmitterState {ShaderType stage; uint32_t wave_size; bool per_invocation_masks;};
uint32_t InitialRegisterValue(const EmitterState&, IR::Register);
void Require(bool value) {if(!value)throw std::runtime_error("NGG vertex ABI regression");}
int main() {
 for(uint32_t wave: {32u,64u}) for(bool invocation: {false,true}) {
  EmitterState vertex {ShaderType::Vertex,wave,invocation};
  const auto s2=InitialRegisterValue(vertex,{IR::RegisterFile::Scalar,2});
  const auto s3=InitialRegisterValue(vertex,{IR::RegisterFile::Scalar,3});
  // Captured prolog builds GS_ALLOC_REQ from S3 low bytes and EXEC from minus S3.
  const auto allocation=(((s3>>8u)&255u)<<12u)|(s3&255u);
  Require(allocation==wave && (s3&(1u<<28u))!=0 && (s2>>12u)==wave);
  Require((~uint64_t{0}>>((-s3)&63u))==(wave==32?0xffffffffull:~uint64_t{0}));
  Require(InitialRegisterValue(vertex,{IR::RegisterFile::Scalar,4})==0);
 }
 for(auto stage: {ShaderType::Compute,ShaderType::Pixel}) {
  EmitterState other {stage,64,false};
  Require(InitialRegisterValue(other,{IR::RegisterFile::Scalar,2})==0);
  Require(InitialRegisterValue(other,{IR::RegisterFile::Scalar,3})==0);
 }
 EmitterState native {ShaderType::Vertex,32,false},single {ShaderType::Vertex,64,true};
 Require(InitialRegisterValue(native,{IR::RegisterFile::Exec,0})==0xffffffffu);
 Require(InitialRegisterValue(native,{IR::RegisterFile::Exec,1})==0);
 Require(InitialRegisterValue(single,{IR::RegisterFile::Exec,0})==1);
 Require(InitialRegisterValue(single,{IR::RegisterFile::Exec,1})==0);
 std::puts("NGG wave32/wave64 allocation, vertex masks and other stages passed");
}
