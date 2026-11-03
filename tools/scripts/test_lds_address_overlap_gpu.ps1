param([string]$EmitterPath)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
if(!$EmitterPath){$EmitterPath=Join-Path $out 'native_mesh_emitter_test.exe'}
$EmitterPath=(Resolve-Path -LiteralPath $EmitterPath).Path
$saved=@{}
foreach($name in @('PSEMU_ASTRO_MESH_OUTPUT_PROBE','PSEMU_TEST_MESH_COUNT','VK_INSTANCE_LAYERS','VK_LAYER_PATH')){
 $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\mesh_gpu_smoke.cpp" "/Fe$out\lds_overlap_gpu.exe" "/Fo$out\lds_overlap_gpu.obj" /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Wide LDS GPU runner compilation failed'}
 $env:PSEMU_ASTRO_MESH_OUTPUT_PROBE=$null;$env:PSEMU_TEST_MESH_COUNT=$null
 $env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
 if(Test-Path -LiteralPath "$env:VULKAN_SDK\Bin\VkLayer_khronos_validation.json"){$env:VK_LAYER_PATH="$env:VULKAN_SDK\Bin"}
 foreach($mode in @('default','multiwave')){
  if($mode -eq 'default'){& $EmitterPath}else{& $EmitterPath multiwave}
  if($LASTEXITCODE){throw 'Wide LDS fixture emission failed'}
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 native_mesh_lane_backing.spv
  if($LASTEXITCODE){throw 'Wide LDS fixture SPIR-V invalid'}
  & .\lds_overlap_gpu.exe native_mesh_lane_backing.spv mesh_fragment.spv
  if($LASTEXITCODE){throw "Wide LDS overlap GPU failed: $mode"}
 }
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 native_mesh_point_push.spv
 if($LASTEXITCODE){throw 'Wide LDS 128-invocation SPIR-V invalid'}
 & .\lds_overlap_gpu.exe native_mesh_point_push.spv mesh_fragment.spv point-push
 if($LASTEXITCODE){throw 'Wide LDS four-guest-wave GPU failed'}
 Write-Output 'PASS B64/B96/B128 LDS: all destination/address overlaps, sparse EXEC, both banks, repeated PC, native32/64/128 invocation controls'
}finally{
 foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
 Pop-Location
}
