$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force|Out-Null
Push-Location $out
try{
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\include" "$root\tests\guest_rwlock_test.cpp" /Fe:guest_rwlock_test.exe
 if($LASTEXITCODE){throw 'Guest RW-lock regression compilation failed'}
 $p=[Diagnostics.Process]::new()
 $p.StartInfo.FileName=Join-Path $out 'guest_rwlock_test.exe'
 $p.StartInfo.UseShellExecute=$false
 $p.StartInfo.CreateNoWindow=$true
 $p.StartInfo.RedirectStandardOutput=$true
 $p.StartInfo.RedirectStandardError=$true
 $null=$p.Start()
 if(!$p.WaitForExit(15000)){$p.Kill();throw 'Guest RW-lock regression timed out'}
 $p.StandardOutput.ReadToEnd()|Write-Output
 $p.StandardError.ReadToEnd()|Write-Output
 if($p.ExitCode -ne 0){throw "Guest RW-lock regression failed: $($p.ExitCode)"}
 $p.Dispose()
}finally{Pop-Location}
