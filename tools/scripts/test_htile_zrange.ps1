$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests\htile-zrange'
New-Item -ItemType Directory -Path $out -Force | Out-Null
& clang-cl /nologo /std:c++20 /O2 /EHsc /MT "/I$root\gpu\src" "$root\tests\htile_zrange_test.cpp" "/Fe$out\htile_zrange_test.exe" "/Fo$out\htile_zrange_test.obj"
if($LASTEXITCODE -ne 0){throw 'HTile Z-range CPU compilation failed'}
& "$out\htile_zrange_test.exe"
if($LASTEXITCODE -ne 0){throw 'HTile Z-range CPU regression failed'}

$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\textureCache.cpp")
$a=$source.IndexOf('bool TextureCache::ReadHtileZRangeSnapshot(')
$b=$source.IndexOf('bool TextureCache::PublishImageForBufferRead(',$a)
if($a -lt 0 -or $b -le $a){throw 'Missing production HTile snapshot transaction'}
[IO.File]::WriteAllText("$out\htile_zrange_transaction_production.inc",$source.Substring($a,$b-$a))
& clang-cl /nologo /std:c++20 /O2 /EHsc /MT "/I$out" "/I$root\gpu\src" "$root\tests\htile_zrange_transaction_test.cpp" "/Fe$out\htile_zrange_transaction_test.exe" "/Fo$out\htile_zrange_transaction_test.obj"
if($LASTEXITCODE -ne 0){throw 'HTile snapshot transaction compilation failed'}
& "$out\htile_zrange_transaction_test.exe"
if($LASTEXITCODE -ne 0){throw 'HTile snapshot transaction failed'}
