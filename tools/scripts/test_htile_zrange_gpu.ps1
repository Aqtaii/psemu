param([string]$CapturedShaderPath)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\htile-zrange-gpu'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$oldLayers=$env:VK_LAYER_PATH
try {
 $sdkLayers=Join-Path $env:VULKAN_SDK 'Bin'
 if(Test-Path -LiteralPath (Join-Path $sdkLayers 'VkLayer_khronos_validation.json')){$env:VK_LAYER_PATH=$sdkLayers}
 $spirv=Join-Path $out 'reference.spv'
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\htile_zrange_gpu.comp" -o $spirv
 if($LASTEXITCODE){throw 'HTile reference shader compilation failed'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "/I$root\gpu\src" "$root\tests\htile_zrange_gpu_test.cpp" "/Fe$out\test.exe" "/Fo$out\test.obj" /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'HTile Vulkan fixture compilation failed'}
 $shaders=@($spirv)
 if($CapturedShaderPath){$shaders+=(Resolve-Path -LiteralPath $CapturedShaderPath).Path}
 foreach($shader in $shaders) {
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 $shader
  if($LASTEXITCODE){throw "HTile shader validation failed: $shader"}
  & "$out\test.exe" $shader
  if($LASTEXITCODE){throw "HTile Vulkan regression failed: $shader"}
 }
}finally{$env:VK_LAYER_PATH=$oldLayers}
