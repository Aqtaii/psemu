$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Slice([string]$source,[string]$start,[string]$end) {
 $a=$source.IndexOf($start);$b=$source.IndexOf($end,$a+1)
 if($a -lt 0 -or $b -le $a){throw "Production extraction failed: $start"}
 return $source.Substring($a,$b-$a)
}
$defs=[IO.File]::ReadAllText("$root\gpu\src\graphics\guest_gpu\gpu_defs.h")
$info=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageInfo.h")
$tile=[IO.File]::ReadAllText("$root\gpu\src\graphics\guest_gpu\tile.cpp")
$header=@'
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <stdexcept>
#include <type_traits>
#include <vector>
#define KYTY_PROFILER_FUNCTION()
#define EXIT(...) throw std::runtime_error("invalid conversion")
#define EXIT_IF(x) do {if(x) EXIT();} while(0)
namespace vk {enum class Format {eUndefined,eR32Sfloat,eR32Uint,eB10G11R11UfloatPack32,eR8G8B8A8Unorm,eR8G8B8A8Srgb,eB8G8R8A8Unorm,eB8G8R8A8Srgb};}
namespace Common {struct LockGuard {explicit LockGuard(int&) {}};}
struct Job {template<class F> void Execute(F f) {f();} void Wait() {}};
struct TilerTest {int m_mutex;Job m_job1;};
TilerTest test_tiler{};auto* g_tiler=&test_tiler;
namespace Prospero {
template<class T> constexpr auto GpuEnumValue(T x) {return static_cast<std::underlying_type_t<T>>(x);}
'@
foreach($name in @('TileMode','ImageType','BufferFormat')) {
 $enum=[regex]::Match($defs,"enum class $name : uint32_t \{[\s\S]*?\};").Value
 if(!$enum){throw "Missing production enum $name"};$header+="`n$enum`n"
}
$header+='uint32_t NumBytesPerElement(uint32_t format) {return format==GpuEnumValue(BufferFormat::k8UInt)?1:format==GpuEnumValue(BufferFormat::k16UInt)?2:4;} }'
$header+="`n"+(Slice $info 'struct ImageInfo {' '// Common image-to-buffer copy description.')
$header+="`n[[nodiscard]] inline bool`n"+(Slice $info 'IsCoherentBufferImageRead(' '[[nodiscard]] inline BufferImageWrite')
$header+="`n[[nodiscard]] inline constexpr bool`n"+(Slice $info 'IsSupportedStandard64RenderTarget(' '[[nodiscard]] inline constexpr bool IsTiledRenderTarget')
$header+="`n"+(Slice $info '[[nodiscard]] inline constexpr bool IsTiledRenderTarget(' '[[nodiscard]] inline constexpr DepthTransitionSource')
$header+="`n"+(Slice $tile 'static constexpr uint32_t GetStandard64KB32XPart(' 'struct Standard64KB16Tables')
$header+="`n"+(Slice $tile 'static void TileConvertLinearToTiledStandard64KB32Impl(' 'void TileConvertTiledToLinearStandard64KB16(')
$header+="`n#define EXIT_NOT_IMPLEMENTED(x) EXIT_IF(x)`n"
$header+="`n"+(Slice $tile 'static bool Gen5Thin64KBBlockSizeFromElementBytes(' 'struct Gen5MipTailLocation')
$header+="`ntemplate <typename T>`n"+(Slice $tile 'static uint32_t Gen5RenderTargetOffsetInBlock(' 'static const Standard64KB16Tables& GetStandard64KB16Tables(')
$header+="`n"+(Slice $tile 'void TileConvertLinearToTiledRenderTarget32PreservePadding(' 'void TileConvertLinearToTiledRenderTarget(')
$header+="`n"+(Slice $tile 'static constexpr uint32_t Depth64KB8XOffsetBytes' 'void TileConvertTiledToLinearStandard4KB(')
$shader=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\shader.h")
$views=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageView.h")
$header+="`n"+(Slice $shader 'constexpr uint32_t DstSel(' 'inline uint8_t GetDstSel(')
$header+="`n"+(Slice $views '[[nodiscard]] inline bool IsSupportedStorageSwizzle(' '[[nodiscard]] inline bool IsSupportedStorageDepthTile(')
$header+=([string][char]10)+'constexpr uint64_t TRACKER_PAGE_SIZE=4096;'+([string][char]10)
$header+=Slice $info 'enum class BufferImageBinding' 'enum class StorageBufferRebind'
$header+=Slice $info '[[nodiscard]] inline bool ImageRangeOverlaps(' '[[nodiscard]] inline SampledOverlap ClassifySampledOverlap('
$header+=Slice $info 'struct DepthTargetInfo {' 'struct DepthFormatPolicy {'
$header+=Slice $info '// A clean sampled stencil plane' '[[nodiscard]] inline DepthOverlap ClassifyDepthOverlap('
$header+=Slice $info '[[nodiscard]] inline BufferImageWrite' '[[nodiscard]] inline StorageBufferRebind'
$header+=([string][char]10)+"[[nodiscard]] inline bool"+([string][char]10)+(Slice $info 'CanReconstructStorageForRenderTarget(' '[[nodiscard]] inline DepthOverlap ClassifyDepthTargetOverlap(')
$bindingDefs=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\shaderBindings.h")
$descriptorDefs=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\descriptors.cpp")
$header+=Slice $bindingDefs 'struct ShaderTextureResource {' 'struct ShaderSamplerResource {'
$header+=Slice $descriptorDefs 'static bool IsSupportedDepthTextureEncoding(' 'static void ValidateDepthTargetBinding('
$header+=[IO.File]::ReadAllText("$root\tests\render_target_publication_fixture.h")
$buffers=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\bufferCache.cpp")
$header+=Slice $buffers 'bool CanMergeBufferCacheQueueMask(' 'struct BufferCache::CachedBuffer'
$header+=Slice $buffers 'void BufferCache::PublishReconstructedImageBacking(' 'void BufferCache::ValidateGpuAccess('
$header+=Slice $buffers 'void BufferCache::RefreshCpuImageFragmentsLocked(' 'BufferImageCopySource BufferCache::ObtainBufferForImage('
$tiler=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\tiler.cpp")
$header+=Slice $tiler 'void TileDepthStorageMipsPreservePadding(' 'void Tiler::TileImage(void* dst, const void* src, const ImageInfo& info)'
$header+=[regex]::Match($info,'enum class StorageSampledOverlap : uint8_t \{[^}]+\};').Value
$header+=Slice $info '[[nodiscard]] inline bool IsR32UintFloatReinterpretation(' '[[nodiscard]] inline bool IsRgba16UintFloatReinterpretation('
$rgbaStart=$info.IndexOf('[[nodiscard]] inline bool IsRgba8SrgbReinterpretation(')
$rgbaStart=$info.IndexOf('[[nodiscard]] inline bool IsRgba8SrgbReinterpretation(',$rgbaStart+1)
$rgbaEnd=$info.IndexOf('[[nodiscard]]',$rgbaStart+10)
if($rgbaStart -lt 0 -or $rgbaEnd -le $rgbaStart){throw 'Production RGBA reinterpretation missing'}
$header+=$info.Substring($rgbaStart,$rgbaEnd-$rgbaStart)
$header+=Slice $info '[[nodiscard]] inline StorageSampledOverlap' '[[nodiscard]] inline HostWriteOverlap'
$decoder=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\ShaderDecoder.h")
$ir=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\ShaderIR.h")
$dimension=[regex]::Match($decoder,'enum class ImageDimension : uint32_t \{[\s\S]*?\};').Value
$resourceKind=[regex]::Match($ir,'enum class ResourceKind \{[\s\S]*?\};').Value
if(!$dimension -or !$resourceKind){throw 'Production image enums missing'}
$header+='namespace ShaderRecompiler { namespace Decoder {'+$dimension+'} namespace IR {'+$resourceKind+'struct ImageResource {ResourceKind kind;Decoder::ImageDimension dimension;bool read=false,written=true,atomic=false;};}}'
$header+=Slice $views '[[nodiscard]] inline bool IsSupportedStorageDepthTile(' '[[noreturn]] inline void UnsupportedColorView('
$header+=Slice $descriptorDefs 'static bool IsValidTextureMipView(' 'void ValidateStorageTexture('
$source=Join-Path $out 'depth_storage_production.cpp'
[IO.File]::WriteAllText($source,$header+"`n"+[IO.File]::ReadAllText("$root\tests\depth_storage_test.cpp"),[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $source /Fe:depth_storage_test.exe
 if($LASTEXITCODE -ne 0){throw 'Depth storage test compilation failed'}
 & .\depth_storage_test.exe
 if($LASTEXITCODE -ne 0){throw 'Depth storage regression failed'}
} finally {Pop-Location}
