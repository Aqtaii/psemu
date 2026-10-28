param(
 [Parameter(Mandatory=$true)][string]$LoaderPath,
 [Parameter(Mandatory=$true)][string]$MediaPath,
 [Parameter(Mandatory=$true)][string]$ScratchDirectory
)
$ErrorActionPreference='Stop'
foreach($taskPath in @($LoaderPath,$MediaPath,$ScratchDirectory)) {
 if (![IO.Path]::IsPathRooted($taskPath)) { throw 'All test paths must be absolute' }
}
New-Item -ItemType Directory -Path $ScratchDirectory -Force | Out-Null
Push-Location -LiteralPath $ScratchDirectory
try {
 & $LoaderPath --avplayer-self-test $MediaPath $ScratchDirectory
 if($LASTEXITCODE -ne 0){throw ('AvPlayer self-test failed: '+$LASTEXITCODE)}
} finally {Pop-Location}