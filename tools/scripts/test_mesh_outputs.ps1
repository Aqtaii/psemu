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
 $validator=Join-Path $env:VULKAN_SDK 'Bin\spirv-val.exe'
 & $validator --target-env vulkan1.2 .\mesh_outputs.spv
 if($LASTEXITCODE -ne 0){throw 'Mesh output SPIR-V validation failed'}
 & $validator --target-env vulkan1.2 .\mesh_fragment.spv
 if($LASTEXITCODE -ne 0){throw 'Mesh fragment validation failed'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\mesh_gpu_smoke.cpp" /Fe:mesh_gpu_smoke.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE -ne 0){throw 'Mesh GPU smoke compilation failed'}
 Write-Output 'PASS: Mesh interfaces validate; GPU runner compiled but not executed on CI'
} finally {Pop-Location}
