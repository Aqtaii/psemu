$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\magic_enum\include" "$root\tests\guest_address_lookup_test.cpp" "$root\gpu\src\graphics\shader\recompiler\SpirvBuilder.cpp" "$root\gpu\src\graphics\shader\recompiler\BindingLayout.cpp" "$root\gpu\src\graphics\shader\recompiler\ScalarProvenance.cpp" "$root\gpu\src\graphics\shader\recompiler\SrtPatcher.cpp" "/Fe:guest_address_lookup.exe"
 if($LASTEXITCODE -ne 0){throw 'Guest address lookup compilation failed'}
 & .\guest_address_lookup.exe
 if($LASTEXITCODE -ne 0){throw 'Guest address lookup semantic regression failed'}
 $validator=Join-Path $env:VULKAN_SDK 'Bin\spirv-val.exe'
 if(!(Test-Path -LiteralPath $validator)){throw 'SPIR-V validator missing'}
 & $validator --target-env vulkan1.1 .\guest_address_lookup.spv
 if($LASTEXITCODE -ne 0){throw 'Guest address lookup SPIR-V validation failed'}
 Write-Output 'PASS: production address lookup module validates for Vulkan 1.1'
} finally {Pop-Location}
