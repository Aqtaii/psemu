param([switch]$ReuseObjects,[string]$CapturedShaderPath)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\interpolation-parameters'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
$options=@('/nologo','/std:c++20','/O2','/Gy','/EHsc','/MD','/DFMT_HEADER_ONLY=1','/D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING',"/I$root\gpu\src","/I$root\gpu\generated","/I$root\gpu\3rdparty\SPIRV-Headers\include","/I$root\gpu\3rdparty\fmt\include","/I$root\gpu\3rdparty\magic_enum\include")
Push-Location $out
try {
 $sources=@("$root\tests\interpolation_parameter_test.cpp")
 foreach($name in @('SpirvEmitter','SpirvBuilder','BindingLayout','ResourceMaterialization','SrtWalker','SrtPatcher','ResourceTracking','ShaderInfoCollection','ScalarProvenance','ShaderCFG','ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')){$sources+="$dir\$name.cpp"}
 $sources+=(Get-ChildItem -LiteralPath "$dir\spirvEmitter" -Filter '*.cpp').FullName
 foreach($name in @('ShaderIR','ShaderIRMemory','ShaderIROpcodes','ShaderIRLog')){$sources+="$dir\shaderIR\$name.cpp"}
 $sources+="$root\gpu\src\graphics\guest_gpu\gpu_format.cpp"
 if(!$ReuseObjects){& clang-cl @options /c @sources}
 if($LASTEXITCODE){throw 'Interpolation production object compilation failed'}
 $fixedObjects=@($sources | ForEach-Object {Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($_)+'.obj')})
 if($ReuseObjects){
  & clang-cl @options /c $sources[0] "/Fo$($fixedObjects[0])"
  if($LASTEXITCODE){throw 'Interpolation fixture recompilation failed'}
  $latestHeader=Get-ChildItem -LiteralPath "$root\gpu\src" -Recurse -File -Filter '*.h' |
   Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  for($i=0;$i -lt $sources.Count;$i++){
   if(!(Test-Path -LiteralPath $fixedObjects[$i]) -or
       (Get-Item -LiteralPath $sources[$i]).LastWriteTimeUtc -gt (Get-Item -LiteralPath $fixedObjects[$i]).LastWriteTimeUtc -or
       $latestHeader.LastWriteTimeUtc -gt (Get-Item -LiteralPath $fixedObjects[$i]).LastWriteTimeUtc){
    throw "Stale/missing production object; run without -ReuseObjects: $($sources[$i])"
   }
  }
 }
 & clang-cl @options $fixedObjects /Fe:fixed.exe /link /OPT:REF
 if($LASTEXITCODE){throw 'Interpolation production pipeline compilation failed'}
 & .\fixed.exe @(@($CapturedShaderPath) | Where-Object {$_})
 if($LASTEXITCODE){throw 'Interpolation boundary/IR regression failed'}

 foreach($i in 0..4) {
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 ("interpolation_"+$i+".spv")
  if($LASTEXITCODE){throw 'Interpolation SPIR-V validation failed'}
 }
}finally{Pop-Location}
