$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\debug.cpp")
function Extract([string]$signature) {
 $start=$source.IndexOf($signature)
 if($start -lt 0){throw "Missing production printer: $signature"}
 $end=$source.IndexOf("`n}",$start)
 if($end -lt 0){throw "Unterminated production printer: $signature"}
 return $source.Substring($start,$end+2-$start)
}
$fixture=@'
#define FMT_HEADER_ONLY
#include <fmt/printf.h>
#include <cstdio>
#include <stdexcept>
#include "graphics/guest_gpu/hardwareContext.h"
#define LOGF(...) throw std::runtime_error("optional logger used")
namespace Libs::Graphics {
'@
foreach($signature in @('template <typename... Args>','static void ZPrint(','static void RcPrint(','static void DPrint(','void depth_state_print(')) {
 $fixture+="`n"+(Extract $signature)+"`n"
}
$depthSource=[IO.File]::ReadAllText("$root\gpu\src\graphics\host_gpu\renderer\depthRenderTarget.cpp")
$wrapperStart=$depthSource.IndexOf('template <typename... Args>')
$wrapperEnd=$depthSource.IndexOf("`n}",$wrapperStart)
if($wrapperStart -lt 0 -or $wrapperEnd -lt 0){throw 'Missing production fatal-state bridge'}
$fixture+='[[noreturn]] static void DepthFatal(const char*, ...) {throw std::runtime_error("expected terminal failure");}'+"`n"
$fixture+=$depthSource.Substring($wrapperStart,$wrapperEnd+2-$wrapperStart)+"`n"
$fixture+=@'
}
int main() {
 Libs::Graphics::HW::Context hw;
 auto z=hw.GetDepthRenderTarget();
 z.z_info.plane_compression=5;
 z.z_info.num_mip_levels=3;
 z.z_read_base_addr=0x123456780000ull;
 hw.SetDepthRenderTarget(z);
 try {Libs::Graphics::DepthFatalState(hw, "fixture");}
 catch(const std::runtime_error& e) {
  if(std::string(e.what())=="expected terminal failure") return 0;
  throw;
 }
 return 1;
}
'@
$cpp=Join-Path $out 'depth_fault_print_test.cpp'
$exe=Join-Path $out 'depth_fault_print_test.exe'
$obj=Join-Path $out 'depth_fault_print_test.obj'
$capture=Join-Path $out 'depth_fault_print_test.stderr.txt'
[IO.File]::WriteAllText($cpp,$fixture)
& clang-cl /nologo /std:c++20 /EHsc /O2 /MT "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" $cpp "/Fe$exe" "/Fo$obj"
if($LASTEXITCODE -ne 0){throw 'Depth printer fixture compilation failed'}
$previous=[Environment]::GetEnvironmentVariable('PSEMU_GPU_LOG','Process')
try {
 [Environment]::SetEnvironmentVariable('PSEMU_GPU_LOG','0','Process')
 $p=Start-Process -FilePath $exe -WindowStyle Hidden -RedirectStandardError $capture -Wait -PassThru
 if($p.ExitCode -ne 0){throw "Depth printer fixture failed: $($p.ExitCode)"}
}finally{[Environment]::SetEnvironmentVariable('PSEMU_GPU_LOG',$previous,'Process')}
$text=[IO.File]::ReadAllText($capture)
foreach($required in @('[DEPTH-FAULT-STATE]','0x0000123456780000','z_info.plane_compression','0x05','z_info.num_mip_levels','0x03','resummarize_enable','copy_sample','stencil_enable','depth_clear_value')) {
 if(!$text.Contains($required)){throw "Missing fatal snapshot field/value: $required"}
}
Write-Output 'Fatal depth state output: PASS with optional logger forbidden and PSEMU_GPU_LOG=0'
