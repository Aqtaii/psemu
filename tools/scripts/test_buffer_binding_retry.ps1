param([switch]$RunGpu)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
function CloseBrace([string]$text,[int]$open){
 $depth=0
 for($i=$open;$i -lt $text.Length;$i++){
  if($text[$i] -eq '{'){$depth++}
  if($text[$i] -eq '}'){$depth--;if($depth -eq 0){return $i+1}}
 }
 throw 'Unbalanced production source'
}
$compute=Get-Content -LiteralPath "$root\gpu\src\graphics\host_gpu\renderer\renderCompute.cpp" -Raw
$marker=([string][char]9)+'for (;;) {'
$start=$compute.IndexOf($marker,$compute.IndexOf('ShaderBufferDataOverride htile_input;'))
$end=CloseBrace $compute ($compute.IndexOf('{',$start))
$compute=$compute.Substring($start,$end-$start)
$draw=Get-Content -LiteralPath "$root\gpu\src\graphics\host_gpu\renderer\renderDraw.cpp" -Raw
$start=$draw.IndexOf($marker,$draw.IndexOf('RenderDepthInfo pipeline_depth'))
$end=$draw.IndexOf('FlushBounceCopies(state->vk_buffer, false);',$start)
$draw=$draw.Substring($start,$end-$start)+'active->device.Write(active->bound);break;'+[Environment]::NewLine+'}'
function OldLoop([string]$source){
 $start=$source.IndexOf('if (g_render_ctx->GetBufferCache()->GetBindingEpoch() != binding_epoch)')
 if($start -lt 0){throw 'Expected production owner-epoch guard'}
 $end=CloseBrace $source ($source.IndexOf('{',$start))
 return $source.Remove($start,$end-$start)
}
$common=@'
 CommandBuffer* buffer=&active->command;
 const uint64_t submit_id=1;
 ShaderContext shader_context;ShaderContext* sh_ctx=&shader_context;
 InputInfo input_info;Program program;
 uint32_t thread_group_x=32,thread_group_y=1,thread_group_z=1;
 std::span<const uint32_t> cs_shader;
 ShaderBufferDataOverride htile_input;const bool has_htile_input=false;
'@
$draw_common=@'
 CommandBuffer* buffer=&active->command;
 const uint64_t submit_id=1;
 DrawState value;value.merged_mesh=active->mesh;DrawState* state=&value;
 Draw draw;struct {uint32_t first_vertex=0;}emit;
 int pipeline_depth=0,context=0,topology=0,index_source=0;int*ctx=&context;
 ShaderContext shader_context;ShaderContext* sh_ctx=&shader_context;
 const bool log_pipeline_phase=false,set_bind_debug=false,set_auto_debug=false;
'@
function FunctionSource([string]$name,[string]$declarations,[string]$body){
 $nl=[Environment]::NewLine
 return "static void $name(){"+$nl+$declarations+$nl+$body+$nl+'}'+$nl
}
$generated=FunctionSource 'RunComputeFixed' $common $compute
$generated+=FunctionSource 'RunComputeOld' $common (OldLoop $compute)
$generated+=FunctionSource 'RunDrawFixed' $draw_common $draw
$generated+=FunctionSource 'RunDrawOld' $draw_common (OldLoop $draw)
Set-Content -LiteralPath "$out\buffer_binding_production_loops.inc" -Value $generated
Push-Location $out
try{
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\buffer_binding_retry_test.comp" -o buffer_binding_retry_test.spv
 if($LASTEXITCODE){throw 'Buffer binding fixture shader compilation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 buffer_binding_retry_test.spv
 if($LASTEXITCODE){throw 'Buffer binding fixture SPIR-V invalid'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$out" "/I$env:VULKAN_SDK\Include" "$root\tests\buffer_binding_retry_test.cpp" /Fe:buffer_binding_retry_test.exe /Fo:buffer_binding_retry_test.obj /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Production buffer binding loop fixture compilation failed'}
 & .\buffer_binding_retry_test.exe --cpu buffer_binding_retry_test.spv
 if($LASTEXITCODE){throw 'Production buffer binding CPU transaction regression failed'}
 if($RunGpu){
  $saved=$env:VK_LAYER_PATH
  try{
   if(Test-Path -LiteralPath "$env:VULKAN_SDK\Bin\VkLayer_khronos_validation.json"){$env:VK_LAYER_PATH="$env:VULKAN_SDK\Bin"}
   & .\buffer_binding_retry_test.exe --gpu buffer_binding_retry_test.spv
   if($LASTEXITCODE){throw 'Production buffer binding Vulkan transaction regression failed'}
  }finally{$env:VK_LAYER_PATH=$saved}
 }
}finally{Pop-Location}
