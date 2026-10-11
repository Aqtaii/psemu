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
namespace vk {enum class Format {eUndefined};}
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
$header+="`n"+(Slice $tile 'static constexpr uint32_t Depth64KB8XOffsetBytes' 'void TileConvertTiledToLinearStandard4KB(')
$shader=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\shader.h")
$views=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageView.h")
$header+="`n"+(Slice $shader 'constexpr uint32_t DstSel(' 'inline uint8_t GetDstSel(')
$header+="`n"+(Slice $views '[[nodiscard]] inline bool IsSupportedStorageSwizzle(' '[[nodiscard]] inline bool IsSupportedStorageDepthTile(')
$header+=([string][char]10)+'constexpr uint64_t TRACKER_PAGE_SIZE=4096;'+([string][char]10)
$header+=Slice $info 'enum class BufferImageBinding' 'enum class StorageBufferRebind'
$header+=Slice $info '[[nodiscard]] inline bool ImageRangeOverlaps(' '[[nodiscard]] inline SampledOverlap ClassifySampledOverlap('
$header+=Slice $info '[[nodiscard]] inline BufferImageWrite' '[[nodiscard]] inline StorageBufferRebind'
$header+=[IO.File]::ReadAllText("$root\tests\render_target_publication_fixture.h")
$buffers=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\bufferCache.cpp")
$header+=Slice $buffers 'void BufferCache::PublishReconstructedImageBacking(' 'void BufferCache::ValidateGpuAccess('
$source=Join-Path $out 'depth_storage_production.cpp'
[IO.File]::WriteAllText($source,$header+"`n"+[IO.File]::ReadAllText("$root\tests\depth_storage_test.cpp"),[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $source /Fe:depth_storage_test.exe
 if($LASTEXITCODE -ne 0){throw 'Depth storage test compilation failed'}
 & .\depth_storage_test.exe
 if($LASTEXITCODE -ne 0){throw 'Depth storage regression failed'}
} finally {Pop-Location}
