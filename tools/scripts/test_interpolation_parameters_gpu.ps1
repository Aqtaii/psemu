
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\interpolation-parameters'
Push-Location $out
try {
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\interpolation_parameter_gpu.vert" -o interpolation_vertex.spv
 if($LASTEXITCODE){throw 'Interpolation vertex shader failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 interpolation_vertex.spv
 if($LASTEXITCODE){throw 'Vertex SPIR-V validation failed'}
 & clang-cl /nologo /std:c++20 /EHsc /O2 /MD "/I$env:VULKAN_SDK\Include" "$root\tests\interpolation_parameter_gpu_test.cpp" /Fe:interpolation_gpu.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Interpolation GPU compilation failed'}
 foreach($i in 0..4){
  & .\interpolation_gpu.exe interpolation_vertex.spv ("interpolation_"+$i+".spv") $i
  if($LASTEXITCODE){throw "Interpolation GPU failed case $i"}
 }
}finally{Pop-Location}
