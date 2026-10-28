param([Parameter(Mandatory=$true)][string]$ToolsDirectory)
$ErrorActionPreference='Stop'
if (![IO.Path]::IsPathRooted($ToolsDirectory)) { throw 'ToolsDirectory must be absolute' }
$taskRoot=[IO.Path]::GetFullPath($ToolsDirectory)
$taskRelease='9ac4cfd195f1'
$taskHash='32839a244a418063f6fb4bbe55585bc66ea17a859b920cae0e3aa1eab9398c09'
$taskZip=Join-Path $taskRoot ('ffmpeg-'+$taskRelease+'-windows-x64.zip')
$taskDest=Join-Path $taskRoot ('ffmpeg-'+$taskRelease)
New-Item -ItemType Directory -Path $taskRoot -Force | Out-Null
if (!(Test-Path -LiteralPath $taskZip)) {
 Invoke-WebRequest -Uri ('https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/'+$taskRelease+'/ffmpeg-windows-x64.zip') -OutFile $taskZip
}
if ((Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash -ne $taskHash) { throw 'FFmpeg SHA256 mismatch' }
if (!(Test-Path -LiteralPath (Join-Path $taskDest 'include\libavcodec\avcodec.h'))) {
 Expand-Archive -LiteralPath $taskZip -DestinationPath $taskDest
}
foreach ($taskFile in @('include\libavcodec\avcodec.h','lib\avformat.lib','lib\avcodec.lib','lib\swscale.lib','lib\swresample.lib','lib\avutil.lib','share\ffmpeg\copyright','share\ffmpeg\SOURCE.txt')) {
 if (!(Test-Path -LiteralPath (Join-Path $taskDest $taskFile))) { throw ('FFmpeg package incomplete: '+$taskFile) }
}
Write-Output ('Verified FFmpeg root: '+$taskDest)
Write-Output ('Configure with -DPSEMU_FFMPEG_ROOT='+$taskDest.Replace('\','/'))