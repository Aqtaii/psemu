param([string]$EmitterPath,[switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
if(!$EmitterPath){$EmitterPath=Join-Path $out 'native_mesh_emitter_test.exe'}
$EmitterPath=(Resolve-Path -LiteralPath $EmitterPath).Path
$vars=@{
 PSEMU_ASTRO_MESH_OUTPUT_PROBE='1';PSEMU_ASTRO_MESH_PROBE_INSTANCE='0'
 PSEMU_ASTRO_MESH_PROBE_FIRST_VERTEX='0';PSEMU_ASTRO_MESH_PROBE_GROUPS='1'
 PSEMU_TEST_MESH_COUNT=$null;PSEMU_TEST_MESH_CHECKPOINTS='1'
 PSEMU_ASTRO_MESH_TRACE_PC_0='0xe0';PSEMU_ASTRO_MESH_TRACE_VBASE_0='0'
 PSEMU_ASTRO_MESH_TRACE_PC_1='0x104';PSEMU_ASTRO_MESH_TRACE_VBASE_1='0'
 PSEMU_ASTRO_MESH_TRACE_PC_2='0x104';PSEMU_ASTRO_MESH_TRACE_VBASE_2='224'
 PSEMU_ASTRO_MESH_TRACE_PC_3='0xf8';PSEMU_ASTRO_MESH_TRACE_VBASE_3='0'
 VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation';VK_LAYER_PATH=$env:VK_LAYER_PATH
}
if(Test-Path -LiteralPath "$env:VULKAN_SDK\Bin\VkLayer_khronos_validation.json"){$vars.VK_LAYER_PATH="$env:VULKAN_SDK\Bin"}
$saved=@{}
foreach($name in $vars.Keys){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
Push-Location $out
try {
 foreach($name in $vars.Keys){[Environment]::SetEnvironmentVariable($name,$vars[$name],'Process')}
 if(!$ValidateOnly) {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\mesh_gpu_smoke.cpp" "/Fe$out\mesh_checkpoint_gpu.exe" "/Fo$out\mesh_checkpoint_gpu.obj" /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Register checkpoint GPU runner compilation failed'}
 }
 & $EmitterPath
 if($LASTEXITCODE){throw 'Register checkpoint shader emission failed'}
 foreach($shader in @('native_mesh_output_probe.spv','native_mesh_point_output_probe.spv')) {
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 $shader
  if($LASTEXITCODE){throw "Invalid checkpoint SPIR-V: $shader"}
 }
 if($ValidateOnly){Write-Output 'PASS four bounded register checkpoints: native32 and merged64 SPIR-V validation (GPU execution not requested)';return}
 & .\mesh_checkpoint_gpu.exe native_mesh_output_probe.spv mesh_fragment.spv output-probe
 if($LASTEXITCODE){throw 'Native wave32 register checkpoint GPU check failed'}
 foreach($mode in @('point-output-probe','point-output-miss','point-output-group-miss')) {
  & .\mesh_checkpoint_gpu.exe native_mesh_point_output_probe.spv mesh_fragment.spv $mode
  if($LASTEXITCODE){throw "Virtual wave64 register checkpoint GPU check failed: $mode"}
 }
 Write-Output 'PASS four bounded checkpoints: exact raw NaN bits, scalar and virtual vector banks, sparse EXEC, absent-register masks, untouched nonselected draws, unchanged positions/indices and fault header'
}finally{
 foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
 Pop-Location
}
