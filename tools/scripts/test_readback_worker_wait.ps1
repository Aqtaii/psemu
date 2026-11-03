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
 throw 'Unbalanced production state declaration'
}
$generated=''
foreach($item in @(@('Buffer','bufferCache.cpp'),@('Texture','textureCache.cpp'))){
 $name=$item[0];$source=Get-Content -LiteralPath "$root\gpu\src\graphics\host_gpu\renderer\$($item[1])" -Raw
 $start=$source.IndexOf('enum class State : uint32_t')
 if($start -lt 0){throw 'Missing production worker states'}
 $end=CloseBrace $source ($source.IndexOf('{',$start))
 $generated+=$source.Substring($start,$end-$start).Replace('enum class State',"enum class $($name)State")+';'+[Environment]::NewLine
 $match=[regex]::Match($source,'WaitForReadbackWork\(state,[^;]+\)')
 if(-not $match.Success -or $match.Value -notmatch 'State::Claimed'){throw 'Production worker must wait through claimed payload ownership'}
 $call=$match.Value
 $old=$call -replace ',\s*State::Claimed',''
 if($old -eq $call){throw 'Missing exact old control'}
 $generated+="template<class Atomic> auto Wait$($name)Production(Atomic& state){using State=$($name)State;return Libs::Graphics::$call;}"+[Environment]::NewLine
 $generated+="template<class Atomic> auto Wait$($name)Old(Atomic& state){using State=$($name)State;return Libs::Graphics::$old;}"+[Environment]::NewLine
}
Set-Content -LiteralPath "$out\readback_wait_calls.inc" -Value $generated
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\gpu\src" "/I$out" "$root\tests\readback_worker_wait_test.cpp" /Fe:readback_worker_wait_test.exe /Fo:readback_worker_wait_test.obj
 if($LASTEXITCODE){throw 'Readback publication fixture compilation failed'}
 & .\readback_worker_wait_test.exe
 if($LASTEXITCODE){throw 'Readback publication regression failed'}
}finally{Pop-Location}
