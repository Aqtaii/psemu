$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\include" "$root\tests\astro_static_tls_test.cpp" /Fe:astro_static_tls_test.exe
 if($LASTEXITCODE){throw 'Astro static TLS compilation failed'}
 & .\astro_static_tls_test.exe
 if($LASTEXITCODE){throw 'Astro static TLS regression failed'}
} finally {Pop-Location}