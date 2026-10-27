$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /Od /EHsc /MD "/I$root\include" "$root\tests\guest_memory_query_test.cpp" /Fe:guest_memory_query_test.exe
 if($LASTEXITCODE){throw 'Memory-query regression compilation failed'}
 & .\guest_memory_query_test.exe
 if($LASTEXITCODE){throw 'Memory-query regression failed'}
} finally {Pop-Location}
