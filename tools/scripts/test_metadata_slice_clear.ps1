$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\metadata-slice-clear'
New-Item -ItemType Directory -Path $out -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $out 'cmake_config.h'),("#pragma once"+[Environment]::NewLine+"#define KYTY_COMPILER 2"+[Environment]::NewLine))
function Slice([string]$source,[string]$start,[string]$end) {
 $a=$source.IndexOf($start)
 if($a -lt 0){throw "Missing production function $start"}
 $b=$source.IndexOf($end,$a+1)
 if($b -le $a){throw "Missing production function end $end"}
 return $source.Substring($a,$b-$a)
}
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\textureCache.cpp")
$info=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageInfo.h")
$production=(Slice $info '[[nodiscard]] inline bool ImagePageRangesOverlap(' '[[nodiscard]] inline SampledOverlap ClassifySampledOverlap(')
$production+=(Slice $source 'bool TextureCache::ResolveMetaClearRangeLocked(' 'bool TextureCache::HasMetaOverlapLocked(')
$production+=(Slice $source 'bool TextureCache::IsMetaCleared(' 'bool TextureCache::ClearMeta(')
[IO.File]::WriteAllText((Join-Path $out 'metadata_slice_clear_production.inc'),$production)
& clang-cl /nologo /std:c++20 /EHsc /O2 /MT "/I$out" "/I$root\gpu\src" "$root\tests\metadata_slice_clear_test.cpp" "/Fe$out\metadata_slice_clear_test.exe" "/Fo$out\metadata_slice_clear_test.obj"
if($LASTEXITCODE -ne 0){throw 'Metadata slice production transaction compilation failed'}
& "$out\metadata_slice_clear_test.exe"
if($LASTEXITCODE -ne 0){throw 'Metadata slice production transaction failed'}
