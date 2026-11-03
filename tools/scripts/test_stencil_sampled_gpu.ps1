$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\stencil-sampled-gpu'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$oldLayers=$env:VK_LAYER_PATH
try{
 if(Test-Path -LiteralPath "$env:VULKAN_SDK\Bin\VkLayer_khronos_validation.json"){$env:VK_LAYER_PATH="$env:VULKAN_SDK\Bin"}
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\stencil_sampled_gpu.comp" -o "$out\stencil.spv"
 if($LASTEXITCODE){throw 'Stencil shader compilation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 "$out\stencil.spv"
 if($LASTEXITCODE){throw 'Stencil SPIR-V invalid'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\stencil_sampled_gpu_test.cpp" "/Fe$out\test.exe" "/Fo$out\test.obj" /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Stencil GPU compilation failed'}
 & "$out\test.exe" "$out\stencil.spv"
 if($LASTEXITCODE){throw 'Stencil GPU regression failed'}
}finally{$env:VK_LAYER_PATH=$oldLayers}
