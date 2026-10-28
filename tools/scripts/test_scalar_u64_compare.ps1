param([string]$Capture='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
if($Capture){$Capture=(Resolve-Path -LiteralPath $Capture).Path}
$dir=Join-Path $root 'gpu\src\graphics\shader\recompiler'
Push-Location $out
try {
 $sources=@("$root\tests\scalar_u64_compare_test.cpp")
 foreach($name in @('SpirvEmitter','SpirvBuilder','BindingLayout','ResourceMaterialization','SrtWalker','SrtPatcher','ResourceTracking','ShaderInfoCollection','ScalarProvenance','ShaderCFG','ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')) {$sources+="$dir\$name.cpp"}
 $sources+=(Get-ChildItem -LiteralPath "$dir\spirvEmitter" -Filter '*.cpp').FullName
 foreach($name in @('ShaderIR','ShaderIRMemory','ShaderIROpcodes','ShaderIRLog')) {$sources+="$dir\shaderIR\$name.cpp"}
 $sources+="$root\gpu\src\graphics\guest_gpu\gpu_format.cpp"
 & clang-cl /nologo /std:c++20 /O2 /Gy /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\SPIRV-Headers\include" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" @sources /Fe:scalar_u64_compare_test.exe /link /OPT:REF
 if($LASTEXITCODE -ne 0){throw 'Scalar U64 production pipeline compilation failed'}
 $captureArgs=@();if($Capture){$captureArgs=@($Capture)}
 & .\scalar_u64_compare_test.exe @captureArgs
 if($LASTEXITCODE){throw 'Scalar U64 production regression failed'}
 foreach($name in @('scalar_eq_u64.spv','scalar_lg_u64.spv')){
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 $name
  if($LASTEXITCODE){throw "Scalar U64 SPIR-V validation failed: $name"}
 }
}finally{Pop-Location}
