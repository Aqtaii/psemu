$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$source=Get-Content -LiteralPath "$root\gpu\src\graphics\guest_gpu\graphicsRun.cpp" -Raw
$a=$source.IndexOf('bool TestWaitRegMemValue(')
$b=$source.IndexOf('static void YieldCommandProcessorWait(',$a)
$c=$source.IndexOf('template <typename T>',$b)
$d=$source.IndexOf('template void CommandProcessor::WaitRegMem<uint32_t>',$c)
if($a -lt 0 -or $b -lt $a -or $c -lt $b -or $d -lt $c){throw 'Missing production wait boundaries'}
$body=$source.Substring($c,$d-$c)
$block=[regex]::Match($body,'(?s)\s*if \(!submitted_pending_work\) \{.*?\n\t\t\}')
if(-not $block.Success){throw 'Missing production submit-before-wait block'}
$old=$body.Remove($block.Index,$block.Length).Replace('CommandProcessor::WaitRegMem(','CommandProcessor::WaitRegMemOld(')
$generated=$source.Substring($a,$b-$a)+$body+$old
Set-Content -LiteralPath "$out\wait_mem_submit.inc" -Value $generated
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$out" "$root\tests\wait_mem_submit_test.cpp" /Fe:wait_mem_submit_test.exe /Fo:wait_mem_submit_test.obj
 if($LASTEXITCODE){throw 'Production wait fixture compilation failed'}
 & .\wait_mem_submit_test.exe
 if($LASTEXITCODE){throw 'Production wait completion regression failed'}
}finally{Pop-Location}
