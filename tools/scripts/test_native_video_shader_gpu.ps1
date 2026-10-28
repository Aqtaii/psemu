param(
 [Parameter(Mandatory=$true)][string]$VertexSpirv,
 [Parameter(Mandatory=$true)][string]$PixelSpirv
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vertex=(Resolve-Path -LiteralPath $VertexSpirv).Path
$pixel=(Resolve-Path -LiteralPath $PixelSpirv).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
# Input is the real production video pair: VS efd4cb1c, PS 7313c4a5.
# Those caller-owned captures are intentionally absent from this repository and CI.
foreach($inputShader in @($vertex,$pixel)) {
 $sha=[Security.Cryptography.SHA256]::Create()
 try{$digest=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($inputShader))).Replace('-','')}finally{$sha.Dispose()}
 Write-Output "[ASTRO-VIDEO-SHADER-PROBE] input=$inputShader sha256=$digest"
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 $inputShader
 if($LASTEXITCODE){throw "Production SPIR-V validation failed: $inputShader"}
}
Push-Location $out
try {
 & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V --target-env vulkan1.2 "$root\tests\native_video_shader_gpu.vert" -o native_video_shader_gpu_vs.spv
 if($LASTEXITCODE){throw 'Synthetic vertex shader compilation failed'}
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$env:VULKAN_SDK\Include" "$root\tests\native_video_shader_gpu_test.cpp" /Fe:native_video_shader_gpu_test.exe /link "/LIBPATH:$env:VULKAN_SDK\Lib" vulkan-1.lib
 if($LASTEXITCODE){throw 'Video shader Vulkan test compilation failed'}
 $result=& .\native_video_shader_gpu_test.exe native_video_shader_gpu_vs.spv $pixel 2>&1
 $nativeExit=$LASTEXITCODE
 $result | ForEach-Object {Write-Output $_}
 if($nativeExit -or ($result -match 'Validation Error:')){throw 'Production pixel shader with synthetic vertex shader failed'}
 $result=& .\native_video_shader_gpu_test.exe $vertex $pixel native-vertex 2>&1
 $nativeExit=$LASTEXITCODE
 $result | ForEach-Object {Write-Output $_}
 if($nativeExit -or ($result -match 'Validation Error:')){throw 'Production vertex/pixel shader pair failed'}
 $result=& .\native_video_shader_gpu_test.exe $vertex $pixel native-vertex video-quad 2>&1
 $nativeExit=$LASTEXITCODE
 $result | ForEach-Object {Write-Output $_}
 if($nativeExit -or ($result -match 'Validation Error:')){throw 'Production video pair with six-vertex quad and negative viewport failed'}
}finally{Pop-Location}
