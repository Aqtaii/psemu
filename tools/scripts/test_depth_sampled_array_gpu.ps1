
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try{
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\depth_sampled_array_gpu.comp" -o depth_sampled_array_gpu.spv
 if($LASTEXITCODE){throw 'Depth array shader compilation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 depth_sampled_array_gpu.spv
 if($LASTEXITCODE){throw 'Depth array shader validation failed'}
 & clang-cl /nologo /std:c++20 /EHsc /O2 /MD "/I$env:VULKAN_SDK\Include" "$root\tests\depth_sampled_array_gpu_test.cpp" /Fe:depth_sampled_array_gpu_test.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Depth array GPU compilation failed'}
 & .\depth_sampled_array_gpu_test.exe depth_sampled_array_gpu.spv
 if($LASTEXITCODE){throw 'Depth array GPU test failed'}
}finally{Pop-Location}
