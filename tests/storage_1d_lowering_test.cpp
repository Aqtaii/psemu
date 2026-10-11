#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include <array>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
namespace IR=Libs::Graphics::ShaderRecompiler::IR;
namespace Decoder=Libs::Graphics::ShaderRecompiler::Decoder;
namespace Prospero=Libs::Graphics::Prospero;
using IR::DescriptorValue;using IR::ResourceKind;
enum {OpCompositeConstruct=80};
enum class ImageViewKind {Dim2D,Dim2DArray,Dim3D};
struct Builder {
 uint32_t next=10;std::unordered_map<uint32_t,uint32_t> values;
 std::array<uint32_t,2> result{};
 uint32_t AllocateId(){return next++;}
 void AddFunction(std::initializer_list<uint32_t> words) {
  const auto p=words.begin();
  if(words.size()!=5 || p[0]!=OpCompositeConstruct)throw std::runtime_error("Unexpected coordinate op");
  result={values.at(p[3]),values.at(p[4])};
 }
};
struct EmitterState {Builder builder;uint32_t vec2_uint_type=2,vec3_uint_type=3,reads=0;};
uint32_t ConstantU32(EmitterState* s,uint32_t value) {
 const auto id=s->builder.AllocateId();s->builder.values[id]=value;return id;
}
uint32_t EmitImageAddressValueLoad(EmitterState* s,const IR::Instruction&,const IR::Operand&,uint32_t component) {
 ++s->reads;return ConstantU32(s,component==0?7u:0xdeadbeefu);
}
uint32_t ImageViewCoordinateComponents(ImageViewKind view){return view==ImageViewKind::Dim2D?2u:3u;}
static void Check(bool ok){if(!ok)throw std::runtime_error("1D storage lowering regression");}
