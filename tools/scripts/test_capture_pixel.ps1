$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "$root\tests\capture_pixel_test.cpp" /Fe:capture_pixel_test.exe
 if($LASTEXITCODE -ne 0){throw 'Capture pixel test compilation failed'}
 & .\capture_pixel_test.exe
 if($LASTEXITCODE -ne 0){throw 'Capture pixel channel conversion failed'}
 Write-Output 'PASS: RGBA/BGRA8 and packed RGB/BGR10 capture channels'
} finally {Pop-Location}