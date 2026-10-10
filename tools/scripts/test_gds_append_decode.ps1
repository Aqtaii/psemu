$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
$production="$dir\MemoryOps.cpp"
$source=[IO.File]::ReadAllText($production)
$marker='// DS_APPEND/CONSUME address the counter at M0.base + the encoded byte offset.'
$gate=@'
if (inst->gds && (inst->opcode == Opcode::DsAppend || inst->opcode == Opcode::DsConsume) && inst->offset != 0u) {
 SetUnsupported(inst, Family::DS, opcode, "GDS append/consume requires a zero instruction offset");
}
'@
if(!$source.Contains($marker)){throw 'Regression source marker missing'}
$baseline=Join-Path $out 'gds_decoder_old.cpp'
[IO.File]::WriteAllText($baseline,$source.Replace($marker,$gate),[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 foreach($variant in @('fixed','old')) {
  $implementation=if($variant -eq 'fixed'){$production}else{$baseline}
  $define=if($variant -eq 'old'){'/DEXPECT_OLD_GDS_REJECTION=1'}else{'/DEXPECT_OLD_GDS_REJECTION=0'}
  $sources=@("$root\tests\gds_append_decode_test.cpp",$implementation)
  foreach($name in @('ShaderDecoder','ScalarAluOps','VectorAluOps','ImageOps','ExportOps')){$sources+="$dir\$name.cpp"}
  & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1  $define "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources "/Fe:gds_decoder_$variant.exe"
  if($LASTEXITCODE -ne 0){throw 'GDS decoder compilation failed'}
  & ".\gds_decoder_$variant.exe"
  if($LASTEXITCODE -ne 0){throw 'GDS decoder regression failed'}
 }
 $emitter=[IO.File]::ReadAllText("$dir\SpirvEmitter.cpp")
 $begin=$emitter.IndexOf('bool Fail(')
 $end=$emitter.IndexOf('bool ValidateNativeProgram(')
 if($begin -lt 0 -or $end -le $begin){throw 'Emitter contract extraction failed'}
 $contract=$emitter.Substring($begin,$end-$begin)
 $oldContract=$contract.Replace('inst.dst.kind != IR::OperandKind::Register)) ||','inst.dst.kind != IR::OperandKind::Register || (kind == IR::ResourceKind::Gds && inst.memory.offset != 0))) ||')
 if($oldContract -eq $contract){throw 'Old contract regression substitution failed'}
 foreach($variant in @('fixed','old')) {
  $implementation=if($variant -eq 'fixed'){$contract}else{$oldContract}
  $define=if($variant -eq 'old'){'/DEXPECT_OLD_GDS_REJECTION=1'}else{'/DEXPECT_OLD_GDS_REJECTION=0'}
  $prefix='#include "graphics/shader/recompiler/ShaderIR.h"'+[Environment]::NewLine+'namespace Libs::Graphics::ShaderRecompiler::Spirv {'
  $suffix='} bool GdsContract(const Libs::Graphics::ShaderRecompiler::IR::Instruction& inst,std::string* error) {return Libs::Graphics::ShaderRecompiler::Spirv::ValidateInstructionContract(inst,error);}'
  $file=Join-Path $out 'gds_contract_production.cpp'
  [IO.File]::WriteAllText($file,$prefix+$implementation+$suffix,[Text.UTF8Encoding]::new($false))
  & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 $define "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" $file "$root\tests\gds_append_contract_test.cpp" "/Fe:gds_contract_$variant.exe"
  if($LASTEXITCODE -ne 0){throw 'GDS contract compilation failed'}
  & ".\gds_contract_$variant.exe"
  if($LASTEXITCODE -ne 0){throw 'GDS contract regression failed'}
 }
} finally {Pop-Location}
