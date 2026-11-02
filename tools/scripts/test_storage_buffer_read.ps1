$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\storage-buffer-read'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\textureCache.cpp")
$a=$source.IndexOf('bool TextureCache::PublishStorageForBufferRead(')
$b=$source.IndexOf('bool TextureCache::InvalidateMemoryFromGPU(',$a)
if($a -lt 0 -or $b -le $a){throw 'Missing production read publication transaction'}
[IO.File]::WriteAllText("$out\storage_buffer_read_production.inc",$source.Substring($a,$b-$a))
& clang-cl /nologo /std:c++20 /EHsc /O2 /MT "/I$out" "$root\tests\storage_buffer_read_test.cpp" "/Fe$out\test.exe" "/Fo$out\test.obj"
if($LASTEXITCODE -ne 0){throw 'Storage read transaction fixture compilation failed'}
& "$out\test.exe"
if($LASTEXITCODE -ne 0){throw 'Storage read transaction fixture failed'}