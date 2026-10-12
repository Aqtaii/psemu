$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir=Join-Path $root 'gpu\src\graphics\shader\recompiler'
Push-Location $out
try {
 $sources=@("$root\tests\native_mesh_emitter_test.cpp")
 foreach($name in @('SpirvEmitter','SpirvBuilder','BindingLayout','ResourceMaterialization','SrtWalker','ScalarProvenance','ShaderCFG','ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')) {$sources+="$dir\$name.cpp"}
 $sources+=(Get-ChildItem -LiteralPath "$dir\spirvEmitter" -Filter '*.cpp').FullName
 $sources+="$dir\shaderIR\ShaderIRLog.cpp"
 $sources+="$root\gpu\src\graphics\guest_gpu\gpu_format.cpp"
 & clang-cl /nologo /std:c++20 /O2 /Gy /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\SPIRV-Headers\include" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:native_mesh_emitter_test.exe /link /OPT:REF
 if($LASTEXITCODE -ne 0){throw 'Native Mesh emitter compilation failed'}
 & .\native_mesh_emitter_test.exe
 if($LASTEXITCODE -ne 0){throw 'Native Mesh emitter regression failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_lane_backing.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh two-lane backing SPIR-V validation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_upper_outputs.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh upper-half output SPIR-V validation failed'}
 & .\native_mesh_emitter_test.exe multiwave
 if($LASTEXITCODE -ne 0){throw 'Native Mesh multiwave regression failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_buffer_reads.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh buffer reads validation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_point_push.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh runtime point push-data validation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_phased_dispatcher.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh phased dispatcher validation failed'}

 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_upper_outputs_multiwave.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh second-subgroup upper output validation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_emitter_multiwave.spv
 if($LASTEXITCODE -ne 0){throw 'Native Mesh multiwave SPIR-V validation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-dis.exe" .\native_mesh_emitter.spv -o .\native_mesh_emitter.spvasm
 if($LASTEXITCODE -ne 0){throw 'Native Mesh disassembly failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 .\native_mesh_emitter.spv
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath .\native_mesh_emitter.spvasm -TotalCount 45; throw 'Native Mesh emitter SPIR-V validation failed'}
} finally {Pop-Location}
