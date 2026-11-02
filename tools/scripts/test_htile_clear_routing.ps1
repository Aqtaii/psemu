$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\htile-routing'
New-Item -ItemType Directory -Path $out -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $out 'cmake_config.h'),("#pragma once"+[Environment]::NewLine+"#define KYTY_COMPILER 2"+[Environment]::NewLine))
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\renderCompute.cpp")
function ExtractBetween([string]$startText,[string]$endText) {
 $start=$source.IndexOf($startText)
 if($start -lt 0){throw "Missing production function: $startText"}
 $end=$source.IndexOf($endText,$start)
 if($end -lt 0){throw "Missing production function end: $endText"}
 return $source.Substring($start,$end-$start)
}
$production=(ExtractBetween 'static uint64_t BufferDescriptorSize(' 'bool ResolveHtileClearTarget(')+
 (ExtractBetween 'static void ValidateFullHtileClearDispatch(' '// Preserve the exact rejected state')+
 (ExtractBetween 'static bool TryConsumeComputeMetaClear(' 'bool ResolveComputeImageClear(')
$condition='resource.written && meta_addr != 0 && descriptor.Base48() == meta_addr'
if(!$production.Contains($condition)){throw 'Missing fixed metadata-write classification'}
$include=Join-Path $out 'htile_clear_routing_production.inc'
try {
 foreach($old in @($false,$true)) {
  $variant=if($old){'old-control'}else{'fixed'}
  $body=if($old){$production.Replace($condition,'meta_addr != 0 && descriptor.Base48() == meta_addr')}else{$production}
  [IO.File]::WriteAllText($include,$body)
  $exe=Join-Path $out "$variant.exe"
  $options=@('/nologo','/std:c++20','/EHsc','/O2','/MT',"/I$out","/I$root\gpu\src","$root\tests\htile_clear_routing_test.cpp","/Fe$exe","/Fo$out\$variant.obj")
  if($old){$options+='/DEXPECT_OLD_ROUTING'}
  & clang-cl @options
  if($LASTEXITCODE -ne 0){throw "HTile routing compilation failed: $variant"}
  & $exe
  if($LASTEXITCODE -ne 0){throw "HTile routing test failed: $variant"}
 }
}finally{[IO.File]::WriteAllText($include,$production)}