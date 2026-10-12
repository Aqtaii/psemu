$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\SPIRV-Headers\include" "$root\tests\mesh_outputs_test.cpp" "$root\gpu\src\graphics\shader\recompiler\SpirvBuilder.cpp" /Fe:mesh_outputs_test.exe
 if($LASTEXITCODE -ne 0){throw 'Mesh output module compilation failed'}
 & .\mesh_outputs_test.exe
 if($LASTEXITCODE -ne 0){throw 'Mesh output module generation failed'}
 & (Join-Path $env:VULKAN_SDK 'Bin\spirv-val.exe') --target-env vulkan1.2 .\mesh_outputs.spv
 if($LASTEXITCODE -ne 0){throw 'Mesh output SPIR-V validation failed'}
 Write-Output 'PASS: Mesh output interface validates for Vulkan 1.2'
} finally {Pop-Location}
