param([switch]$SkipCpuBuild)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\sopk-immediates'
if(!$SkipCpuBuild){& "$PSScriptRoot\test_sopk_immediates.ps1"}
foreach($file in @('sopk_fixed_fragment.spv','sopk_old_fragment.spv')){
 if(!(Test-Path -LiteralPath "$out\$file")){throw "Missing freshly built comparison fixture: $file"}
}
& "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\sopk_fullscreen.mesh" -o "$out\sopk_fullscreen_mesh.spv"
if($LASTEXITCODE){throw 'SOPK controlled Mesh geometry compilation failed'}
& "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 "$out\sopk_fullscreen_mesh.spv"
if($LASTEXITCODE){throw 'SOPK Mesh SPIR-V validation failed'}
& clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\mesh_gpu_smoke.cpp" "/Fe$out\gpu.exe" "/Fo$out\gpu.obj" /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
if($LASTEXITCODE){throw 'SOPK GPU runner compilation failed'}
& "$out\gpu.exe" "$out\sopk_fullscreen_mesh.spv" "$out\sopk_fixed_fragment.spv" *> "$out\fixed_gpu.txt"
if($LASTEXITCODE){Get-Content "$out\fixed_gpu.txt";throw 'Correct SOPK predicate failed actual Vulkan framebuffer check'}
& "$out\gpu.exe" "$out\sopk_fullscreen_mesh.spv" "$out\sopk_old_fragment.spv" *> "$out\old_gpu.txt"
if($LASTEXITCODE -ne 1){Get-Content "$out\old_gpu.txt";throw 'Old decoder control must fail framebuffer check'}
$fixed=[IO.File]::ReadAllText("$out\fixed_gpu.txt")
$old=[IO.File]::ReadAllText("$out\old_gpu.txt")
if(!$fixed.Contains('correct_pixels=1024 total=1024') -or !$old.Contains('correct_pixels=0 total=1024')){
 throw 'GPU controls did not distinguish the captured comparison'
}
foreach($text in @($fixed,$old)){
 if(!$text.Contains('[MESH-VALIDATION] issues=0 callback=1')){throw 'Missing clean active Vulkan validation evidence'}
}
Write-Output 'SOPK Vulkan predicate: fixed 1024/1024 red pixels; old decoder 0/1024; both active-validation callbacks report zero issues. Controlled geometry/data, not an Astro game frame.'