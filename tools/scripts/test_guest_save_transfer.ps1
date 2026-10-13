$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\include" "$root\tests\guest_save_transfer_test.cpp" /Fe:guest_save_transfer_test.exe
 if($LASTEXITCODE -ne 0){throw 'Guest save transfer compilation failed'}
 & '.\guest_save_transfer_test.exe'
 if($LASTEXITCODE -ne 0){throw 'Guest save transfer regression failed'}
} finally {Pop-Location}
