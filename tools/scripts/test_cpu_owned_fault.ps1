$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$source=Get-Content -LiteralPath "$root\gpu\src\graphics\host_gpu\renderer\gpuResourceManager.cpp" -Raw
$start=$source.IndexOf('bool GpuResourceManager::HandleFault(')
$end=$source.IndexOf('bool GpuResourceManager::IsRegistered(',$start)
if($start -lt 0 -or $end -lt $start){throw 'Missing production fault boundaries'}
Set-Content -LiteralPath "$out\cpu_owned_fault.inc" -Value $source.Substring($start,$end-$start)
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$out" "$root\tests\cpu_owned_fault_test.cpp" /Fe:cpu_owned_fault_test.exe /Fo:cpu_owned_fault_test.obj
 if($LASTEXITCODE){throw 'Production fault fixture compilation failed'}
 & .\cpu_owned_fault_test.exe
 if($LASTEXITCODE){throw 'Production fault regression failed'}
}finally{Pop-Location}
