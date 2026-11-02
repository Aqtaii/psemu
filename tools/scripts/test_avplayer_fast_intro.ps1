param(
 [Parameter(Mandatory=$true)][string]$LoaderPath,
 [Parameter(Mandatory=$true)][string]$MediaPath,
 [Parameter(Mandatory=$true)][string]$ScratchDirectory
)
$ErrorActionPreference='Stop'
foreach($taskPath in @($LoaderPath,$MediaPath,$ScratchDirectory)) {
 if(![IO.Path]::IsPathRooted($taskPath)){throw 'All paths must be absolute'}
}
New-Item -ItemType Directory -Path $ScratchDirectory -Force | Out-Null
Push-Location -LiteralPath $ScratchDirectory
try {
 & $LoaderPath --avplayer-fast-intro-self-test $MediaPath
 if($LASTEXITCODE -ne 0){throw ('Astro fast intro lifecycle test failed: '+$LASTEXITCODE)}
} finally {Pop-Location}