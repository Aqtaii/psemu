param([string]$Loader='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "$root\tests\guest_directory_test.cpp" /Fe:guest_directory_test.exe
 if($LASTEXITCODE -ne 0){throw 'Guest directory test compilation failed'}
 & .\guest_directory_test.exe
 if($LASTEXITCODE -ne 0){throw 'Guest directory records/cursor/bounds regression failed'}
 if($Loader) {
  $fixture=Join-Path $out 'directory-fixture'
  New-Item -ItemType Directory -Path (Join-Path $fixture 'rooms') -Force | Out-Null
  [IO.File]::WriteAllText((Join-Path $fixture 'astro.spx'),'test fixture')
  & $Loader --directory-self-test $fixture
  if($LASTEXITCODE -ne 0){throw 'Loader directory enumeration regression failed'}
 }
 Write-Output 'PASS: Kyty directory ABI,512-byte blocks, types, names, EOF and bounds'
} finally {Pop-Location}