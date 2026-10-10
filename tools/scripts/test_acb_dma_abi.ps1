$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\libs\agc.cpp")
function Slice([string]$start,[string]$end) {
 $a=$source.IndexOf($start);$b=$source.IndexOf($end,$a+1)
 if($a -lt 0 -or $b -le $a){throw 'ACB source extraction failed'}
 return $source.Substring($a,$b-$a)
}
$fixture=[IO.File]::ReadAllText("$root\tests\acb_dma_abi_test.cpp")
$marker='// PRODUCTION DMA BUILDERS'
$code=$fixture.Replace($marker,(Slice 'uint32_t* KYTY_SYSV_ABI GraphicsDcbDmaData(' 'uint32_t* KYTY_SYSV_ABI GraphicsDcbJump(')+(Slice 'uint32_t* KYTY_SYSV_ABI GraphicsAcbDmaData(' 'uint32_t* KYTY_SYSV_ABI GraphicsAcbCopyData('))
if($code -eq $fixture){throw 'Missing fixture marker'}
$file=Join-Path $out 'acb_dma_production.cpp'
[IO.File]::WriteAllText($file,$code,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $file /Fe:acb_dma_abi_test.exe
 if($LASTEXITCODE -ne 0){throw 'ACB ABI test compilation failed'}
 & .\acb_dma_abi_test.exe
 if($LASTEXITCODE -ne 0){throw 'ACB ABI test failed'}
} finally {Pop-Location}
