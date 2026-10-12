$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\gpu\src" "/I$root\gpu\generated" "$root\tests\gs_user_data_address_test.cpp" /Fe:gs_user_data_address_test.exe
 if($LASTEXITCODE -ne 0){throw 'GS user data address compilation failed'}
 & '.\gs_user_data_address_test.exe'
 if($LASTEXITCODE -ne 0){throw 'GS user data address regression failed'}
} finally {Pop-Location}
