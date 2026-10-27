$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force|Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\bufferCache.cpp")
$begin=$source.IndexOf('void BufferCache::RefreshCpuImageFragmentsLocked(')
$middle=$source.IndexOf('bool BufferCache::IsCachedRegionCpuModified(',$begin)
$obtain=$source.IndexOf('BufferImageCopySource BufferCache::ObtainBufferForImage(',$middle)
$end=$source.IndexOf('namespace {',$obtain)
if($begin-lt 0 -or $middle-le $begin -or $obtain-le $middle -or $end-le $obtain){throw 'Production image source extraction failed'}
$ranges=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\rangeSet.h").Replace('#include "common/assert.h"','')
$text="#include <stdexcept>`n#define EXIT(...) throw std::runtime_error(`"production error`")`n"+$ranges
$text+=[IO.File]::ReadAllText("$root\tests\buffer_image_source_fixture.h")
$text+=$source.Substring($begin,$middle-$begin)+$source.Substring($obtain,$end-$obtain)
$text+=[IO.File]::ReadAllText("$root\tests\buffer_image_source_test.cpp")
$cpp=Join-Path $out 'buffer_image_source_production.cpp'
[IO.File]::WriteAllText($cpp,$text,[Text.UTF8Encoding]::new($false))
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $cpp /Fe:buffer_image_source_test.exe
 if($LASTEXITCODE-ne 0){throw 'Image source test compilation failed'}
 & .\buffer_image_source_test.exe
 if($LASTEXITCODE-ne 0){throw 'Image source regression failed'}
}finally{Pop-Location}
