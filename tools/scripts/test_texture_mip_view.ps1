$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Slice([string]$source,[string]$start,[string]$end){$a=$source.IndexOf($start);$b=$source.IndexOf($end,$a+1);if($a -lt 0 -or $b -le $a){throw "Missing production region: $start"};$source.Substring($a,$b-$a)}
$tile=[IO.File]::ReadAllText("$root\gpu\src\graphics\guest_gpu\tile.cpp")
$header=@"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/shader/shaderBindings.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace Libs::Graphics;
#define EXIT_NOT_IMPLEMENTED(x) do{if(x)throw std::runtime_error("invalid layout");}while(0)
"@
$header+="`n"+(Slice $tile 'static uint32_t IntLog2(' 'static uint32_t CalcLinearBlockWidth(')
$header+=Slice $tile 'static bool Gen5Thin64KBBlockSizeFromElementBytes(' 'static uint32_t Gen5Standard4KBOffsetInBlock('
$header+='void ProductionTile27Layout(uint32_t format,uint32_t width,uint32_t height,uint32_t pitch,uint32_t levels,uint32_t tile,TileSizeAlign* total_size,TileSizeOffset* level_sizes,TilePaddedSize* padded_size) {'
$a=$tile.IndexOf('bytes_per_element != 0 && tile == 27 && levels > 1)');if($a -lt 0){throw 'Tile27 branch missing'}
$a=$tile.LastIndexOf('if (const uint32_t bytes_per_element',$a);$b=$tile.IndexOf('if (auto bytes_per_block =',$a)
if($a -lt 0 -or $b -le $a){throw 'Tile27 range missing'}
$header+=$tile.Substring($a,$b-$a)+'throw std::runtime_error("unsupported fixture layout");}'
$source=Join-Path $out 'texture_mip_view_production.cpp'
[IO.File]::WriteAllText($source,$header+"`n"+[IO.File]::ReadAllText("$root\tests\texture_mip_view_test.cpp"),[Text.UTF8Encoding]::new($false))
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" $source "$root\gpu\src\graphics\guest_gpu\gpu_format.cpp" /Fe:texture_mip_view_test.exe
 if($LASTEXITCODE){throw 'Texture mip backing test compilation failed'}
 & .\texture_mip_view_test.exe
 if($LASTEXITCODE){throw 'Texture mip backing test failed'}
}finally{Pop-Location}
