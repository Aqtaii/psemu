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
#include <array>
#include <stdexcept>
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#define LOGF(...) throw std::runtime_error("optional logger used")
namespace Libs::Graphics {
'@
foreach($signature in @('template <typename... Args>','static void ZPrint(','static void RcPrint(','static void DPrint(','void depth_state_print(')) {
 $fixture+="`n"+(Extract $signature)+"`n"
}
$handlers=[IO.File]::ReadAllText("$root\gpu\src\graphics\guest_gpu\command_processor\pm4Handlers.cpp")
$directStart=$handlers.IndexOf('KYTY_HW_CTX_PARSER(HwCtxSetRenderControl)')
$directEnd=$handlers.IndexOf("`n}",$directStart)
$indirectStart=$handlers.IndexOf('g_hw_ctx_indirect_func[Pm4::DB_RENDER_CONTROL] = [](')
$indirectStart=$handlers.IndexOf('[](',$indirectStart)
$indirectEnd=$handlers.IndexOf("`n`t};",$indirectStart)
if($directStart -lt 0 -or $directEnd -lt 0 -or $indirectStart -lt 0 -or $indirectEnd -lt 0){throw 'Missing production render-control parsers'}
$fixture+=@'
struct TestCP {HW::Context* context;HW::Context* GetCtx(){return context;}};
#define EXIT_NOT_IMPLEMENTED(x) do {if(x) throw std::runtime_error("unsupported parser input");} while(0)
#define KYTY_HW_CTX_PARSER(name) uint32_t name(TestCP* cp,uint32_t cmd_id,uint32_t cmd_offset,const uint32_t* buffer)
'@
$fixture+="`n"+$handlers.Substring($directStart,$directEnd+2-$directStart)+"`n"
$indirect=$handlers.Substring($indirectStart,$indirectEnd+4-$indirectStart).Replace('KYTY_HW_CTX_INDIRECT_ARGS','TestCP* cp,uint32_t value')
$fixture+='const auto ParseIndirectRenderControl='+$indirect+"`n"
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
 Libs::Graphics::TestCP cp{&hw};
 auto fields=[](const Libs::Graphics::HW::RenderControl& c){return std::array<uint32_t,8>{
  c.raw,c.depth_clear_enable,c.stencil_clear_enable,c.resummarize_enable,
  c.stencil_compress_disable,c.depth_compress_disable,c.copy_centroid,c.copy_sample};};
 for(const uint32_t raw:{0u,0x80002000u,0xffffffffu,0x80000fffu}) {
  Libs::Graphics::HwCtxSetRenderControl(&cp,0xc0016900,Libs::Graphics::Pm4::DB_RENDER_CONTROL,&raw);
  const auto direct=fields(hw.GetRenderControl());
  if(direct[0]!=raw)throw std::runtime_error("direct PM4 raw bits lost");
  Libs::Graphics::ParseIndirectRenderControl(&cp,raw);
  if(fields(hw.GetRenderControl())!=direct)throw std::runtime_error("direct/indirect PM4 mismatch");
  if(raw==0x80002000u && direct!=std::array<uint32_t,8>{raw,0,0,0,0,0,0,0})
   throw std::runtime_error("unmodeled bits changed known fields");
  if(raw==0xffffffffu && direct!=std::array<uint32_t,8>{raw,1,1,1,1,1,1,15})
   throw std::runtime_error("known PM4 fields changed");
 }

 auto control=hw.GetRenderControl();control.raw=0x80002000u;hw.SetRenderControl(control);
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
foreach($required in @('[DEPTH-FAULT-STATE]','0x0000123456780000','z_info.plane_compression','0x05','z_info.num_mip_levels','0x03','resummarize_enable','db_render_control_raw','0x80002000','copy_sample','stencil_enable','depth_clear_value')) {
 if(!$text.Contains($required)){throw "Missing fatal snapshot field/value: $required"}
}
Write-Output 'Render-control raw direct/indirect parsing and fatal depth state output: PASS with optional logger forbidden and PSEMU_GPU_LOG=0'
