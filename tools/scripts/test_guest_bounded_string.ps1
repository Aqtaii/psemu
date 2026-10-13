$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\include" "$root\tests\guest_bounded_string_test.cpp" /Fe:guest_bounded_string_test.exe
 if($LASTEXITCODE -ne 0){throw 'Guest bounded string compilation failed'}
 & '.\guest_bounded_string_test.exe'
 if($LASTEXITCODE -ne 0){throw 'Guest bounded string regression failed'}
} finally {Pop-Location}
