$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\depthRenderTarget.h")
$start=$source.IndexOf('struct StencilAttachmentUsage {');$end=$source.IndexOf('struct RenderDepthInfo {',$start)
if($start -lt 0 -or $end -le $start){throw 'Missing production stencil attachment policy'}
$fixture=$source.Substring($start,$end-$start)
$fixture+='struct RenderDepthInfo {bool depth_load_clear_enable=false,stencil_clear_enable=false,depth_write_enable=false,stencil_test_enable=false;struct Mask {uint32_t writeMask=0;} stencil_dynamic_front,stencil_dynamic_back;};'
$fixture+="`n"+'#define EXIT_IF(x) do {if(x) throw std::runtime_error("invalid fixture");} while(0)'+"`n"
$start=$source.IndexOf('inline bool depth_attachment_read_only(');$end=$source.IndexOf("`n}",$start)
if($start -lt 0 -or $end -le $start){throw 'Missing production read-only policy'}
$fixture+=$source.Substring($start,$end+2-$start)
[IO.File]::WriteAllText((Join-Path $out 'depth_attachment_usage_fixture.h'),$fixture)
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /EHsc /O2 /MT "/I$root\gpu\src" "/I$root\gpu\generated" "/I$out" "$root\tests\depth_attachment_usage_test.cpp" /Fe:depth_attachment_usage_test.exe /Fo:depth_attachment_usage_test.obj
 if($LASTEXITCODE -ne 0){throw 'Stencil attachment regression compilation failed'}
 & .\depth_attachment_usage_test.exe
 if($LASTEXITCODE -ne 0){throw 'Stencil attachment regression failed'}
} finally {Pop-Location}
