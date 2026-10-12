$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
Push-Location $out
try {
 $sources=@("$root\tests\es_front_decode_test.cpp")
 foreach($name in @('ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps','ShaderCFG')){$sources+="$dir\$name.cpp"}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:es_front_decode_test.exe
 if($LASTEXITCODE -ne 0){throw 'ES front decoder compilation failed'}
 & '.\es_front_decode_test.exe'
 if($LASTEXITCODE -ne 0){throw 'ES front decoder regression failed'}
} finally {Pop-Location}
