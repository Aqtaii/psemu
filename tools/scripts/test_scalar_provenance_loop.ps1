$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$production=Join-Path $root 'gpu\src\graphics\shader\recompiler\ScalarProvenance.cpp'
$source=[IO.File]::ReadAllText($production)
$old=$source.Replace('incoming.size() == 1 && *phi == ScalarProvenance::Undefined','incoming.size() == 1')
if($old -eq $source){throw 'Regression baseline substitution failed'}
$baseline=Join-Path $out 'scalar_provenance_old.cpp'
[IO.File]::WriteAllText($baseline,$old,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 foreach($variant in @('fixed','old')) {
  $implementation=if($variant -eq 'fixed'){$production}else{$baseline}
  $exe=Join-Path $out "scalar_provenance_$variant.exe"
  & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" "$root\tests\scalar_provenance_loop_test.cpp" $implementation "/Fe:$exe"
  if($LASTEXITCODE -ne 0){throw 'Scalar provenance test compilation failed'}
  $stdout=Join-Path $out "scalar_provenance_$variant.log"
  $proc=Start-Process -FilePath $exe -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout
  $finished=$proc.WaitForExit(5000)
  if(!$finished){Stop-Process -Id $proc.Id;$proc.WaitForExit()}
  if($variant -eq 'fixed') {
   if(!$finished -or $proc.ExitCode -ne 0){throw 'Fixed scalar provenance did not converge'}
   Get-Content -LiteralPath $stdout
  } elseif($finished) {throw 'Old scalar provenance unexpectedly converged; regression fixture invalid'}
  else {Write-Output 'PASS: old implementation reproduces non-convergence within bounded timeout'}
 }
} finally {Pop-Location}
