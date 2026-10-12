param([string]$EsCapture='', [string]$GsCapture='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
if(($EsCapture -eq '') -ne ($GsCapture -eq '')){throw 'Supply both ES and GS captures'}
if($EsCapture -ne ''){$EsCapture=(Resolve-Path -LiteralPath $EsCapture).Path; $GsCapture=(Resolve-Path -LiteralPath $GsCapture).Path}
Push-Location $out
try {
 $sources=@("$root\tests\es_front_decode_test.cpp")
 foreach($name in @('ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps','ShaderCFG','ScalarProvenance','SrtWalker','SrtPatcher','ResourceTracking','ResourceMaterialization','BindingLayout')){$sources+="$dir\$name.cpp"}
 foreach($name in @('ShaderIR','ShaderIRMemory','ShaderIROpcodes','ShaderIRLog')){$sources+="$dir\shaderIR\$name.cpp"}
 $sources+="$root\gpu\src\graphics\guest_gpu\gpu_format.cpp"
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:es_front_decode_test.exe
 if($LASTEXITCODE -ne 0){throw 'ES front decoder compilation failed'}
 $captureArgs=@()
 if($EsCapture -ne ''){$captureArgs=@($EsCapture,$GsCapture)}
 & '.\es_front_decode_test.exe' @captureArgs
 if($LASTEXITCODE -ne 0){throw 'ES front decoder regression failed'}
} finally {Pop-Location}
