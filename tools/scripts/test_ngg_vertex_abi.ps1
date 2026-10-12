$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\spirvEmitter\spirvEmitterControlFlow.cpp")
$begin=$source.IndexOf('uint32_t InitialRegisterValue(')
$end=$source.IndexOf('uint32_t EmitInitialRegisterValue(',$begin)
if($begin -lt 0 -or $end -le $begin){throw 'NGG initializer extraction failed'}
$prefix='#include "graphics/shader/recompiler/ShaderIR.h"
namespace IR=Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;
struct EmitterState {ShaderType stage; uint32_t wave_size; bool per_invocation_masks;};'
$file=Join-Path $out 'ngg_vertex_abi_production.cpp'
[IO.File]::WriteAllText($file,$prefix+[char]10+$source.Substring($begin,$end-$begin),[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" $file "$root\tests\ngg_vertex_abi_test.cpp" /Fe:ngg_vertex_abi_test.exe
 if($LASTEXITCODE -ne 0){throw 'NGG ABI compilation failed'}
 & '.\ngg_vertex_abi_test.exe'
 if($LASTEXITCODE -ne 0){throw 'NGG ABI regression failed'}
} finally {Pop-Location}
