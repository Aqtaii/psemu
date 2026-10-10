$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" "$root\tests\scalar_address_shift_test.cpp" "$dir\ScalarProvenance.cpp" "$dir\SrtWalker.cpp" "/Fe:scalar_address_shift.exe"
 if($LASTEXITCODE -ne 0){throw 'Scalar address shift compilation failed'}
 & .\scalar_address_shift.exe
 if($LASTEXITCODE -ne 0){throw 'Scalar address shift regression failed'}
} finally {Pop-Location}
