$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
$source=[IO.File]::ReadAllText("$dir\shaderIR\ShaderIRMemory.cpp")
$begin=$source.IndexOf('bool LowerBvhIntersect(')
$end=$source.IndexOf('bool LowerImageOperation(',$begin)
if($begin -lt 0 -or $end -le $begin){throw 'BVH lowering extraction failed'}
$file=Join-Path $out 'bvh_ir_production.cpp'
$prefix='#include "graphics/shader/recompiler/ShaderIR.h"'+[Environment]::NewLine+'namespace Libs::Graphics::ShaderRecompiler::IR {'
[IO.File]::WriteAllText($file,$prefix+$source.Substring($begin,$end-$begin)+'}',[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 $sources=@($file,"$root\tests\bvh_ir_test.cpp")
 foreach($name in @('ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')){$sources+="$dir\$name.cpp"}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:bvh_ir_test.exe
 if($LASTEXITCODE -ne 0){throw 'BVH IR compilation failed'}
 & .\bvh_ir_test.exe
 if($LASTEXITCODE -ne 0){throw 'BVH IR regression failed'}
} finally {Pop-Location}
