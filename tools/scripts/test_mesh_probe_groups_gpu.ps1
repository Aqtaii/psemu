$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
$names=@('PSEMU_ASTRO_MESH_OUTPUT_PROBE','PSEMU_ASTRO_MESH_PROBE_INSTANCE','PSEMU_ASTRO_MESH_PROBE_FIRST_VERTEX','VK_INSTANCE_LAYERS','VK_LAYER_PATH','PSEMU_ASTRO_MESH_PROBE_GROUPS','PSEMU_TEST_MESH_COUNT')
$saved=@{}
foreach($name in $names) {$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
Push-Location $out
try {
 $env:PSEMU_TEST_MESH_COUNT=$null
 $env:PSEMU_ASTRO_MESH_OUTPUT_PROBE='1'
 $env:PSEMU_ASTRO_MESH_PROBE_INSTANCE='0'
 $env:PSEMU_ASTRO_MESH_PROBE_GROUPS='1'
 $env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
 $sdkLayers=Join-Path $env:VULKAN_SDK 'Bin'
 if(Test-Path -LiteralPath (Join-Path $sdkLayers 'VkLayer_khronos_validation.json')) {$env:VK_LAYER_PATH=$sdkLayers}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\mesh_gpu_smoke.cpp" /Fe:mesh_gpu_smoke.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE -ne 0){throw 'Mesh selection GPU runner compilation failed'}
 foreach($first in @('0','32')) {
  $env:PSEMU_ASTRO_MESH_PROBE_FIRST_VERTEX=$first
  & .\native_mesh_emitter_test.exe
  if($LASTEXITCODE -ne 0){throw "Mesh diagnostic emission failed for first_vertex=$first"}
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_point_output_probe.spv
  if($LASTEXITCODE -ne 0){throw 'Grouped Mesh diagnostic SPIR-V validation failed'}
  foreach($mode in @('point-output-probe','point-output-miss','point-output-group-miss','point-output-multigroup')) {
   & .\mesh_gpu_smoke.exe .\native_mesh_point_output_probe.spv .\mesh_fragment.spv $mode
   if($LASTEXITCODE -ne 0){throw "Mesh group GPU regression failed: first_vertex=$first mode=$mode"}
  }
 }
 $env:PSEMU_ASTRO_MESH_PROBE_GROUPS='4'
 foreach($first in @('0','32')) {
  $env:PSEMU_ASTRO_MESH_PROBE_FIRST_VERTEX=$first
  & .\native_mesh_emitter_test.exe
  if($LASTEXITCODE -ne 0){throw 'Batch diagnostic shader emission failed'}
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_point_output_probe.spv
  if($LASTEXITCODE -ne 0){throw 'Batch diagnostic SPIR-V validation failed'}
  & .\mesh_gpu_smoke.exe .\native_mesh_point_output_probe.spv .\mesh_fragment.spv point-output-batch
  if($LASTEXITCODE -ne 0){throw "Independent batch record GPU test failed at vertex$first"}
 }
 $env:PSEMU_ASTRO_MESH_PROBE_GROUPS='1'
 $env:PSEMU_ASTRO_MESH_PROBE_FIRST_VERTEX='0'
 foreach($count in @('0','0x00010003','0xffffffff')) {
  $env:PSEMU_TEST_MESH_COUNT=$count
  & .\native_mesh_emitter_test.exe
  if($LASTEXITCODE -ne 0){throw "Count predicate emission failed: $count"}
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_point_output_probe.spv
  if($LASTEXITCODE -ne 0){throw 'Count predicate SPIR-V validation failed'}
  & .\mesh_gpu_smoke.exe .\native_mesh_point_output_probe.spv .\mesh_fragment.spv point-output-probe
  if($LASTEXITCODE -ne 0){throw "Count predicate capture failed: $count"}
 }
 Write-Output 'PASS: eight single-group cases, two independent four-record batches, three exact count predicates; guards/fault header retained'

} finally {
 foreach($name in $names) {[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
 Pop-Location
}
