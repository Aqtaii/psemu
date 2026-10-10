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
$analysis=[IO.File]::ReadAllText("$dir\spirvEmitter\spirvEmitterAnalysis.cpp")
$collectBegin=$analysis.IndexOf('void CollectRegister(')
$collectEnd=$analysis.IndexOf('bool HasOutput(',$collectBegin)
$compareBegin=$analysis.IndexOf('bool IsCompareOpcode(IR::Opcode op) {')
$compareEnd=$analysis.IndexOf(([string][char]10+'}'),$compareBegin)+3
if($collectBegin -lt 0 -or $collectEnd -le $collectBegin -or $compareBegin -lt 0 -or $compareEnd -le $compareBegin){throw 'Register collection extraction failed'}
$analysisPrefix=@'
#include <algorithm>
namespace BvhRegisterTest {
namespace IR = Libs::Graphics::ShaderRecompiler::IR;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;
struct RegisterBinding { IR::Register reg; uint32_t id; };
struct EmitterState { uint32_t wave_size; };
'@
$analysisTail=@'
}
std::vector<Libs::Graphics::ShaderRecompiler::IR::Register> BvhCollected(const Libs::Graphics::ShaderRecompiler::IR::Instruction& inst) {
 Libs::Graphics::ShaderRecompiler::IR::Program program;
 program.blocks.resize(1);program.blocks[0].instructions.push_back(inst);
 std::vector<BvhRegisterTest::RegisterBinding> bindings;
 BvhRegisterTest::CollectRegisters(program,&bindings);
 std::vector<Libs::Graphics::ShaderRecompiler::IR::Register> result;
 for(const auto& binding:bindings)result.push_back(binding.reg);
 return result;
}
'@
[IO.File]::AppendAllText($file,([string][char]10)+$analysisPrefix+([string][char]10)+$analysis.Substring($collectBegin,$collectEnd-$collectBegin)+$analysis.Substring($compareBegin,$compareEnd-$compareBegin)+([string][char]10)+$analysisTail,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 $sources=@($file,"$root\tests\bvh_ir_test.cpp")
 foreach($name in @('ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')){$sources+="$dir\$name.cpp"}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:bvh_ir_test.exe
 if($LASTEXITCODE -ne 0){throw 'BVH IR compilation failed'}
 & .\bvh_ir_test.exe
 if($LASTEXITCODE -ne 0){throw 'BVH IR regression failed'}
} finally {Pop-Location}
