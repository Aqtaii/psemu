$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\guest_gpu\graphicsRun.cpp")
$start=$source.IndexOf('void CommandProcessor::DmaData(')
$end=$source.IndexOf('void GraphicsRing::Submit(',$start)
if($start -lt 0 -or $end -le $start){throw 'DMA source extraction failed'}
$implementation=$source.Substring($start,$end-$start)
$signature=$implementation.Substring(0,$implementation.IndexOf('{')).Replace('CommandProcessor::','')
$prefix=@'
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <stdexcept>
#define EXIT_NOT_IMPLEMENTED(x) do {if(x)throw std::runtime_error("unsupported");} while(0)
#define EXIT(...) throw std::runtime_error("unsupported")
#define LOGF(...) ((void)0)
namespace Common {struct LockGuard {explicit LockGuard(int&) {}};}
struct Resources {
 int kind=0;uint64_t dst=0,src=0,size=0;uint32_t value=0;bool dst_gds=false,src_gds=false;
 void CopyBuffer(void*,uint64_t d,uint64_t s,uint64_t n,bool dg,bool sg){kind=1;dst=d;src=s;size=n;dst_gds=dg;src_gds=sg;}
 void FillBuffer(void*,uint64_t d,uint64_t n,uint32_t v,bool dg){kind=2;dst=d;size=n;value=v;dst_gds=dg;src_gds=false;}
} resources;
Resources* GetGpuResources(){return &resources;}
struct CommandProcessor {int m_mutex=0;void* CurrentBuffer(){return this;}
'@
$prefix+=$signature+";};`n"
$file=Join-Path $out 'dma_bytes_production.cpp'
[IO.File]::WriteAllText($file,$prefix+$implementation+[IO.File]::ReadAllText("$root\tests\dma_bytes_test.cpp"),[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\gpu\src" $file /Fe:dma_bytes_test.exe
 if($LASTEXITCODE -ne 0){throw 'DMA test compilation failed'}
 & .\dma_bytes_test.exe
 if($LASTEXITCODE -ne 0){throw 'DMA regression failed'}
} finally {Pop-Location}
