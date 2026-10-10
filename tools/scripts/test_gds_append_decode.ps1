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
} finally {Pop-Location}
