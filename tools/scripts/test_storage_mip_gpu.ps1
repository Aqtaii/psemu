$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
$helper=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\spirvEmitter\spirvEmitterImageHelpers.cpp")
$a=$helper.IndexOf('uint32_t InverseStorageSwizzleComponent(');$b=$helper.IndexOf('uint32_t StorageImageSwizzle(',$a)
if($a -lt 0 -or $b -le $a){throw 'Production inverse swizzle not found'}
$source=[IO.File]::ReadAllText("$root\tests\storage_mip_gpu_test.cpp").Replace('// PRODUCTION_INVERSE',$helper.Substring($a,$b-$a))
New-Item -ItemType Directory -Path $out -Force | Out-Null
[IO.File]::WriteAllText("$out\storage_mip_gpu_production.cpp",$source,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\storage_mip_gpu.comp" -o storage_mip_gpu.spv
 if($LASTEXITCODE){throw 'Storage GPU shader compilation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 storage_mip_gpu.spv
 if($LASTEXITCODE){throw 'Storage GPU SPIR-V validation failed'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" storage_mip_gpu_production.cpp /Fe:storage_mip_gpu_test.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Storage mip GPU test compilation failed'}
 foreach($target in @(0,1,7)) {
  & .\storage_mip_gpu_test.exe storage_mip_gpu.spv $target
  if($LASTEXITCODE){throw "Storage mip Vulkan test failed at mip $target"}
 }
} finally {Pop-Location}
