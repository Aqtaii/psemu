param([string]$GpuLibrary='', [switch]$RunGpu)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dir=Join-Path $root 'gpu\src\graphics\shader\recompiler'
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force|Out-Null
if($GpuLibrary -ne ''){$GpuLibrary=(Resolve-Path -LiteralPath $GpuLibrary).Path}
Push-Location $out
try{
 $sources=@("$root\tests\atomic_image_emitter_test.cpp")
 foreach($name in @('SpirvEmitter','SpirvBuilder','BindingLayout','ResourceMaterialization','SrtWalker','SrtPatcher','ResourceTracking','ShaderInfoCollection','ScalarProvenance','ShaderCFG','ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')){$sources+="$dir\$name.cpp"}
 $sources+=(Get-ChildItem -LiteralPath "$dir\spirvEmitter" -Filter '*.cpp').FullName
 foreach($name in @('ShaderIR','ShaderIRMemory','ShaderIROpcodes','ShaderIRLog')){$sources+="$dir\shaderIR\$name.cpp"}
 $sources+="$root\gpu\src\graphics\guest_gpu\gpu_format.cpp"
 if($GpuLibrary -ne ''){$sources=@("$root\tests\atomic_image_emitter_test.cpp",$GpuLibrary)}
 & clang-cl /nologo /std:c++20 /O2 /Gy /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\SPIRV-Headers\include" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:atomic_image_emitter_test.exe /link /OPT:REF
 if($LASTEXITCODE){throw 'Atomic emitter fixture compile failed'}
 & .\atomic_image_emitter_test.exe
 if($LASTEXITCODE){throw 'Atomic production emission failed'}
 & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 atomic_image_emitter.spv
 if($LASTEXITCODE){throw 'Atomic production SPIR-V validation failed'}
 & "$env:VULKAN_SDK\Bin\spirv-dis.exe" atomic_image_emitter.spv -o atomic_image_emitter.spvasm
 if($LASTEXITCODE){throw 'Atomic production SPIR-V disassembly failed'}
 if($RunGpu){
  $gpuLog=& "$PSScriptRoot\test_storage_mip_gpu.ps1" -AtomicEmitterSpirv (Join-Path $out 'atomic_image_emitter.spv') 2>&1
  $gpuExit=$LASTEXITCODE
  $gpuLog|Write-Output
  if($gpuExit -ne 0 -or ($gpuLog|Select-String 'Validation Error|VUID-|Validation Warning')){throw 'Production image atomic GPU regression or validation failed'}
 }
}finally{Pop-Location}
