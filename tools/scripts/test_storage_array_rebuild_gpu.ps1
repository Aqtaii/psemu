$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force|Out-Null
Push-Location $out
try{
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\storage_array_rebuild_gpu.comp" -o storage_array_rebuild_gpu.spv
 if($LASTEXITCODE){throw 'Array rebuild shader compilation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 storage_array_rebuild_gpu.spv
 if($LASTEXITCODE){throw 'Array rebuild SPIR-V validation failed'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\storage_array_rebuild_gpu_test.cpp" /Fe:storage_array_rebuild_gpu_test.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Array rebuild GPU fixture compilation failed'}
 $gpuLog=& .\storage_array_rebuild_gpu_test.exe storage_array_rebuild_gpu.spv 2>&1
 $gpuExit=$LASTEXITCODE
 $gpuLog|Write-Output
 if($gpuExit -ne 0 -or ($gpuLog|Select-String 'Validation Error|VUID-|Validation Warning')){throw 'Array rebuild GPU regression or validation failed'}
}finally{Pop-Location}
