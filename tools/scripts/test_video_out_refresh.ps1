$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Slice([string]$s,[string]$start,[string]$end){
 $a=$s.IndexOf($start);$b=$s.IndexOf($end,$a+1)
 if($a -lt 0 -or $b -le $a){throw "Production extraction failed: $start"}
 return $s.Substring($a,$b-$a)
}
$info=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\imageInfo.h")
$texture=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\textureCache.cpp")
$header="#include <cstdint>"+[char]10+(Slice $info 'enum class VideoOutCompression' 'struct VideoOutInfo')
$header+=[char]10+"#include <cstdint>"+[char]10
$header+="[[nodiscard]] inline constexpr bool`n"+(Slice $info 'CanUseVideoOutNativeWithoutUpload(' 'struct VideoOutPixelFormatInfo')
$header+=[IO.File]::ReadAllText("$root\tests\video_out_refresh_fixture.h")
$header+=Slice $texture 'void TextureCache::RefreshVideoOut(' 'void TextureCache::UnregisterVideoOutSurfaces('
$source=Join-Path $out 'video_out_refresh_production.cpp'
[IO.File]::WriteAllText($source,$header+[IO.File]::ReadAllText("$root\tests\video_out_refresh_test.cpp"),[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $source /Fe:video_out_refresh_test.exe
 if($LASTEXITCODE -ne 0){throw 'VideoOut refresh compilation failed'}
 & .\video_out_refresh_test.exe
 if($LASTEXITCODE -ne 0){throw 'VideoOut refresh regression failed'}
} finally {Pop-Location}
