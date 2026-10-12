$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" "$root\tests\buffer_address_group_test.cpp" /Fe:buffer_address_group_test.exe
 if($LASTEXITCODE -ne 0){throw 'Buffer address group compilation failed'}
 & '.\buffer_address_group_test.exe'
 if($LASTEXITCODE -ne 0){throw 'Buffer address group regression failed'}
} finally {Pop-Location}
