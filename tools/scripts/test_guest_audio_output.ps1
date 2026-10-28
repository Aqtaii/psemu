$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD "/I$root\include" "$root\tests\guest_audio_output_test.cpp" /Fe:guest_audio_output_test.exe
 if($LASTEXITCODE){throw 'Guest AudioOut2 PCM compilation failed'}
 & .\guest_audio_output_test.exe
 if($LASTEXITCODE){throw 'Guest AudioOut2 PCM regression failed'}
}finally{Pop-Location}