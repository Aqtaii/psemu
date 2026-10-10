$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\gpu\src" "$root\tests\guest_address_table_test.cpp" "/Fe:guest_address_table.exe"
 if($LASTEXITCODE -ne 0){throw 'Guest address table compilation failed'}
 & .\guest_address_table.exe
 if($LASTEXITCODE -ne 0){throw 'Guest address table regression failed'}
} finally {Pop-Location}
