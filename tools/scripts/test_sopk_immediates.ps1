param([switch]$ReuseObjects)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\sopk-immediates'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$dir="$root\gpu\src\graphics\shader\recompiler"
$options=@('/nologo','/std:c++20','/O2','/Gy','/EHsc','/MD','/DFMT_HEADER_ONLY=1',"/I$root\gpu\src","/I$root\gpu\generated","/I$root\gpu\3rdparty\SPIRV-Headers\include","/I$root\gpu\3rdparty\fmt\include","/I$root\gpu\3rdparty\magic_enum\include")
Push-Location $out
try {
 $sources=@("$root\tests\sopk_immediates_test.cpp")
 foreach($name in @('SpirvEmitter','SpirvBuilder','BindingLayout','ResourceMaterialization','SrtWalker','SrtPatcher','ResourceTracking','ShaderInfoCollection','ScalarProvenance','ShaderCFG','ShaderDecoder','ScalarAluOps','VectorAluOps','MemoryOps','ImageOps','ExportOps')){$sources+="$dir\$name.cpp"}
 $sources+=(Get-ChildItem -LiteralPath "$dir\spirvEmitter" -Filter '*.cpp').FullName
 foreach($name in @('ShaderIR','ShaderIRMemory','ShaderIROpcodes','ShaderIRLog')){$sources+="$dir\shaderIR\$name.cpp"}
 $sources+="$root\gpu\src\graphics\guest_gpu\gpu_format.cpp"
 if(!$ReuseObjects){& clang-cl @options /c @sources}
 if($LASTEXITCODE){throw 'SOPK production object compilation failed'}
 $fixedObjects=@($sources | ForEach-Object {Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($_)+'.obj')})
 if($ReuseObjects){
  & clang-cl @options /c $sources[0] "/Fo$($fixedObjects[0])"
  if($LASTEXITCODE){throw 'SOPK fixture recompilation failed'}
  for($i=0;$i -lt $sources.Count;$i++){
   if(!(Test-Path -LiteralPath $fixedObjects[$i]) -or
       (Get-Item -LiteralPath $sources[$i]).LastWriteTimeUtc -gt (Get-Item -LiteralPath $fixedObjects[$i]).LastWriteTimeUtc){
    throw "Stale/missing production object; run without -ReuseObjects: $($sources[$i])"
   }
  }
 }
 & clang-cl @options $fixedObjects /Fe:fixed.exe /link /OPT:REF
 if($LASTEXITCODE){throw 'SOPK production pipeline compilation failed'}
 & .\fixed.exe
 if($LASTEXITCODE){throw 'SOPK boundary/IR regression failed'}
 $source=[IO.File]::ReadAllText("$dir\ScalarAluOps.cpp")
 $pattern='opcode >= 0x09u && opcode <= 0x0eu\s*\? static_cast<int32_t>\(word & 0xffffu\)\s*: static_cast<int32_t>\(static_cast<int16_t>\(word & 0xffffu\)\)'
 if(![regex]::IsMatch($source,$pattern)){throw 'Fixed SOPK immediate expression missing'}
 $old=[regex]::Replace($source,$pattern,'static_cast<int16_t>(word & 0xffffu)')
 [IO.File]::WriteAllText("$out\ScalarAluOps_old.cpp",$old)
 & clang-cl @options /c "$out\ScalarAluOps_old.cpp" /Fo:ScalarAluOps_old.obj
 if($LASTEXITCODE){throw 'Old decoder control compilation failed'}
 & clang-cl @options /DEXPECT_OLD_SOPK /c "$root\tests\sopk_immediates_test.cpp" /Fo:sopk_old_test.obj
 if($LASTEXITCODE){throw 'Old predicate control compilation failed'}
 $objects=@($fixedObjects | Where-Object {[IO.Path]::GetFileName($_) -notin @('ScalarAluOps.obj','sopk_immediates_test.obj')}) + @("$out\ScalarAluOps_old.obj","$out\sopk_old_test.obj")
 & clang-cl @options $objects /Fe:old-control.exe /link /OPT:REF
 if($LASTEXITCODE){throw 'Old decoder control link failed'}
 & .\old-control.exe
 if($LASTEXITCODE){throw 'Old decoder reproducer failed'}
 foreach($name in @('sopk_fixed_fragment.spv','sopk_old_fragment.spv')){
  & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.2 $name
  if($LASTEXITCODE){throw "SOPK fragment SPIR-V validation failed: $name"}
 }
}finally{Pop-Location}