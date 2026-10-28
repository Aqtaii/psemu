param(
 [Parameter(Mandatory=$true)][string]$LoaderPath,
 [string]$MediaPath,
 [Parameter(Mandatory=$true)][string]$ScratchDirectory
)
$ErrorActionPreference='Stop'
foreach($taskPath in @($LoaderPath,$ScratchDirectory)) {
 if (![IO.Path]::IsPathRooted($taskPath)) { throw 'Loader and scratch paths must be absolute' }
}
if($MediaPath -and ![IO.Path]::IsPathRooted($MediaPath)) { throw 'Media path must be absolute' }
New-Item -ItemType Directory -Path $ScratchDirectory -Force | Out-Null
$taskGenerated=$null
if(!$MediaPath) {
 $taskGenerated=Join-Path $ScratchDirectory ('avplayer-ci-'+[Guid]::NewGuid().ToString('N')+'.wav')
 $taskStream=[IO.File]::Open($taskGenerated,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
 $taskWriter=[IO.BinaryWriter]::new($taskStream)
 try {
  $taskWriter.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
  $taskWriter.Write([uint32](36+4096));$taskWriter.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
  $taskWriter.Write([uint32]16);$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]2)
  $taskWriter.Write([uint32]48000);$taskWriter.Write([uint32]192000)
  $taskWriter.Write([uint16]4);$taskWriter.Write([uint16]16)
  $taskWriter.Write([Text.Encoding]::ASCII.GetBytes('data'));$taskWriter.Write([uint32]4096)
  for($taskSample=0;$taskSample -lt 1024;$taskSample++){$taskWriter.Write([int16]16384);$taskWriter.Write([int16]-8192)}
 } finally {$taskWriter.Dispose()}
 $MediaPath=$taskGenerated
}
Push-Location -LiteralPath $ScratchDirectory
try {
 & $LoaderPath --avplayer-self-test $MediaPath $ScratchDirectory
 if($LASTEXITCODE -ne 0){throw ('AvPlayer self-test failed: '+$LASTEXITCODE)}
} finally {
 Pop-Location
 if($taskGenerated){Remove-Item -LiteralPath $taskGenerated}
}