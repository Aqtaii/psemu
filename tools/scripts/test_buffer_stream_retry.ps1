$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$source=Get-Content -LiteralPath "$root\gpu\src\graphics\host_gpu\renderer\bufferCache.cpp" -Raw
function CloseBrace([string]$text,[int]$open){
 $depth=0
 for($i=$open;$i -lt $text.Length;$i++){
  if($text[$i] -eq '{'){$depth++}
  if($text[$i] -eq '}'){$depth--;if($depth -eq 0){return $i+1}}
 }
 throw 'Unbalanced production source'
}
$generated=''
foreach($name in @('MergeOverlappingBufferCacheRange','CanMergeBufferCacheQueueMask')){
 $start=$source.IndexOf("bool $name(")
 if($start -lt 0){throw "Missing production helper $name"}
 $end=CloseBrace $source ($source.IndexOf('{',$start))
 $generated+=$source.Substring($start,$end-$start)+[Environment]::NewLine
}
$start=$source.IndexOf('std::pair<VulkanBuffer*, uint64_t> BufferCache::ObtainBuffer(')
$end=$source.IndexOf('const auto texture_pages',$start)
if($start -lt 0 -or $end -lt $start){throw 'Missing production preparation boundaries'}
$prefix=$source.Substring($start,$end-$start)+'return CompleteResident(ctx,begin,end);'+[Environment]::NewLine+'}'+[Environment]::NewLine
$early=$prefix.IndexOf('if (const auto stream = try_cpu_stream(initial_texture_region); stream.first)')
if($early -lt 0){throw 'Missing early stream read'}
$earlyEnd=CloseBrace $prefix ($prefix.IndexOf('{',$early))
$old=$prefix.Remove($early,$earlyEnd-$early)
$generated+=$prefix.Replace('BufferCache::ObtainBuffer(','BufferCache::ObtainFixed(')
$generated+=$old.Replace('BufferCache::ObtainBuffer(','BufferCache::ObtainOld(')
Set-Content -LiteralPath "$out\buffer_stream_preparation.inc" -Value $generated
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$out" "$root\tests\buffer_stream_retry_test.cpp" /Fe:buffer_stream_retry_test.exe /Fo:buffer_stream_retry_test.obj
 if($LASTEXITCODE){throw 'Production stream prefix fixture compilation failed'}
 & .\buffer_stream_retry_test.exe
 if($LASTEXITCODE){throw 'Production stream retry regression failed'}
}finally{Pop-Location}
