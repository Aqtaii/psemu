$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\stencil-sampled'
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
#include "graphics/shader/recompiler/ShaderIR.h"
#include <stdexcept>
using namespace Libs::Graphics;
#undef EXIT
#define EXIT(...) throw std::runtime_error("production view guard")
enum class VulkanImageType{Other,DepthStencil};
struct VulkanImage{
 VulkanImageType type;vk::Format format;uint32_t layers,mip_levels;
 struct{uint32_t width,height;}extent;uint32_t guest_pitch;vk::Image image{};
};
struct DepthStencilVulkanImage:VulkanImage{};
struct GraphicContext{};
struct ImageViewInfo{
 vk::Format format;vk::ImageViewType type;vk::ImageAspectFlags aspect;
 uint32_t base_level,level_count,base_layer,layer_count,swizzle;
 vk::ImageUsageFlags usage=vk::ImageUsageFlagBits::eSampled;
};
struct TextureCache{
 uint32_t calls=0;ImageViewInfo info{};
 vk::ImageView GetImageView(GraphicContext*,VulkanImage*,const ImageViewInfo& value){++calls;info=value;return {};}
 vk::ImageView GetStencilTargetSampledView(GraphicContext*,DepthStencilVulkanImage*,uint32_t);
};
'@
$defs=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\descriptors.cpp")
$views=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageView.cpp")
$source+=Slice $defs 'static bool IsSupportedDepthTextureEncoding(' 'static void ValidateDepthTargetBinding('
$source+=Slice $defs 'bool IsSupportedStencilTargetBinding(' 'static bool IsValidTextureMipView('
$source+=Slice $views 'vk::ImageView TextureCache::GetStencilTargetSampledView(' 'vk::ImageView TextureCache::GetSampledColorView('
$source+=[IO.File]::ReadAllText("$root\tests\stencil_sampled_binding_test.cpp")
[IO.File]::WriteAllText("$out\binding.cpp",$source,[Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$env:VULKAN_SDK\Include" "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" "$out\binding.cpp" "/Fe$out\binding.exe" "/Fo$out\binding.obj"
if($LASTEXITCODE){throw 'Stencil binding compilation failed'}
& "$out\binding.exe"
if($LASTEXITCODE){throw 'Stencil binding regression failed'}

$info=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageInfo.h")
$cache=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\textureCache.cpp")
$body=(Slice $info '[[nodiscard]] inline bool IsDepthTargetRangeCompatible(' '[[nodiscard]] inline bool ImageRangeOverlaps(')
$body+=Slice $cache 'DepthStencilVulkanImage* TextureCache::FindDepthTargetByRange(' 'RenderTextureVulkanImage* TextureCache::FindRenderTargetByRange('
[IO.File]::WriteAllText("$out\stencil_owner_production.inc",$body)
& clang-cl /nologo /std:c++20 /O2 /EHsc /MT "/I$out" "$root\tests\stencil_sampled_owner_test.cpp" "/Fe$out\owner.exe" "/Fo$out\owner.obj"
if($LASTEXITCODE){throw 'Stencil owner compilation failed'}
& "$out\owner.exe"
if($LASTEXITCODE){throw 'Stencil owner regression failed'}
