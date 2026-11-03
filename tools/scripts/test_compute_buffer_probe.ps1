param([switch]$RunGpu)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force -Path $out|Out-Null
# Reuse only the Vulkan test device, not the modeled binding transaction types.
$device=Get-Content -LiteralPath "$root\tests\buffer_binding_retry_test.cpp" -Raw
$end=$device.IndexOf('namespace vk {')
if($end -lt 0){throw 'Vulkan test device boundary missing'}
Set-Content -LiteralPath "$out\compute_probe_device.inc" -Value $device.Substring(0,$end)
Push-Location $out
try {
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\buffer_binding_retry_test.comp" -o compute_buffer_probe_writer.spv
 if($LASTEXITCODE){throw 'Producer probe writer compilation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 compute_buffer_probe_writer.spv
 if($LASTEXITCODE){throw 'Producer probe writer SPIR-V invalid'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$out" "/I$root\gpu\src" "/I$env:VULKAN_SDK\Include" "$root\tests\compute_buffer_probe_test.cpp" /Fe:compute_buffer_probe_test.exe /Fo:compute_buffer_probe_test.obj /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib /delayload:vulkan-1.dll delayimp.lib
 if($LASTEXITCODE){throw 'Producer probe fixture compilation failed'}
 & .\compute_buffer_probe_test.exe --cpu compute_buffer_probe_writer.spv
 if($LASTEXITCODE){throw 'Producer probe CPU bounds regression failed'}
 if($RunGpu){
  $saved=$env:VK_LAYER_PATH
  try{
   if(Test-Path -LiteralPath "$env:VULKAN_SDK\Bin\VkLayer_khronos_validation.json"){$env:VK_LAYER_PATH="$env:VULKAN_SDK\Bin"}
   & .\compute_buffer_probe_test.exe --gpu compute_buffer_probe_writer.spv
   if($LASTEXITCODE){throw 'Producer probe Vulkan before/after regression failed'}
  }finally{$env:VK_LAYER_PATH=$saved}
 }
}finally{Pop-Location}
