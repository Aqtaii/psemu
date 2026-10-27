param([string]$ScratchDirectory='D:\Tools\psemu-build\temp')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$scratch=(Resolve-Path -LiteralPath $ScratchDirectory).Path
if($scratch -notmatch '^[Dd]:\\'){throw 'Scratch directory must be on D:'}
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$before=@(Get-ChildItem -LiteralPath $scratch -Filter 'psemu-dmem-*.tmp' | ForEach-Object Name)
$oldTemp=$env:TEMP;$oldTmp=$env:TMP
try {
 $env:TEMP=$scratch;$env:TMP=$scratch
 Push-Location $out
 try {
  & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\include" "$root\tests\shared_direct_memory_test.cpp" /Fe:shared_direct_memory_test.exe
  if($LASTEXITCODE){throw 'Shared direct-memory compilation failed'}
  & .\shared_direct_memory_test.exe
  if($LASTEXITCODE){throw 'Pagefile-backed regression failed'}
  & .\shared_direct_memory_test.exe file large
  if($LASTEXITCODE){throw 'D-file-backed regression failed'}
 } finally {Pop-Location}
 $left=@(Get-ChildItem -LiteralPath $scratch -Filter 'psemu-dmem-*.tmp' | Where-Object {$_.Name -notin $before})
 if($left.Count){throw 'Test scratch file was not removed on close'}
 Write-Output 'PASS: both backends, sparse 16GiB mapping, commit headroom, protected aliases and delete-on-close'
} finally {$env:TEMP=$oldTemp;$env:TMP=$oldTmp}