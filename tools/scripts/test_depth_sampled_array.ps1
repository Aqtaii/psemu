
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Slice([string]$s,[string]$a,[string]$b){
 $start=$s.IndexOf($a);$end=$s.IndexOf($b,$start+1)
 if($start -lt 0 -or $end -le $start){throw "Missing production region $a"}
 $s.Substring($start,$end-$start)
}
$source=@'
#include <vulkan/vulkan.hpp>
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/shaderBindings.h"
#include <cstdio>
using namespace Libs::Graphics;
// Model only the cache owner and the independently tested pitch dependency.
enum class VulkanImageType {Other,DepthStencil};
struct VulkanImage {VulkanImageType type;uint32_t layers;struct {uint32_t width,height;} extent;uint32_t guest_pitch;};
uint32_t TileGetTexturePitch(uint32_t,uint32_t,uint32_t,uint32_t){return 2048;}
namespace ShaderRecompiler::Decoder {enum class ImageDimension {Dim2D,Dim2DArray};}
namespace ShaderRecompiler::IR {struct ImageResource {ShaderRecompiler::Decoder::ImageDimension dimension;};}
'@
$defs=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\descriptors.cpp")
$header=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\descriptors.h")
$source+=Slice $header 'struct TargetTextureViewInfo {' '[[nodiscard]] TargetTextureViewInfo'
$source+=Slice $defs 'TargetTextureViewInfo ResolveTargetTextureView(' 'bool IsSupportedSampledVideoOutView('
$source+=Slice $defs 'bool IsSupportedDepthTargetDescriptor(' 'static void ValidateDepthTargetBinding('
$source+=[IO.File]::ReadAllText("$root\tests\depth_sampled_array_test.cpp")
[IO.File]::WriteAllText("$out\depth_sampled_array_production.cpp",$source,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /EHsc /O2 /MD /DFMT_HEADER_ONLY=1 "/I$env:VULKAN_SDK\Include" "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" depth_sampled_array_production.cpp /Fe:depth_sampled_array_test.exe
 if($LASTEXITCODE){throw 'Depth array CPU build failed'}
 & .\depth_sampled_array_test.exe
 if($LASTEXITCODE){throw 'Depth array CPU regression failed'}
} finally {Pop-Location}
