$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /EHsc /O2 /MT "/I$root\gpu\src" "/I$root\gpu\generated" "$root\tests\depth_texture_compatibility_test.cpp" /Fe:depth_texture_compatibility_test.exe /Fo:depth_texture_compatibility_test.obj
 if($LASTEXITCODE -ne 0){throw 'Depth compatibility compilation failed'}
 & .\depth_texture_compatibility_test.exe
 if($LASTEXITCODE -ne 0){throw 'Depth compatibility regression failed'}
} finally {Pop-Location}
