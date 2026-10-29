#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include "graphics/guest_gpu/hardwareContext.h"
#include "depth_attachment_usage_fixture.h"
static void Require(bool ok) {if(!ok) throw std::runtime_error("stencil attachment usage regression");}
int main() {
 // Captured 81-shader failure: 960x540 D32, no stencil plane, stale enabled
 // front/back stencil tests, ALWAYS comparisons, KEEP ops and 0xff masks.
 Libs::Graphics::HW::DepthStencilInfo s;
 Libs::Graphics::HW::DepthControl d;
 Libs::Graphics::HW::RenderControl c;
 s.format=0;d.stencil_enable=true;d.z_enable=true;d.z_write_enable=false;
 d.backface_enable=true;d.stencilfunc=d.stencilfunc_bf=7;
 auto usage=ResolveStencilAttachmentUsage(s.format,d.stencil_enable,c.stencil_clear_enable);
 Require(!usage.bound&&!usage.test&&!usage.clear);
 RenderDepthInfo depth;
 depth.depth_write_enable=d.z_write_enable;
 depth.stencil_test_enable=usage.test;depth.stencil_clear_enable=usage.clear;
 depth.stencil_dynamic_front.writeMask=depth.stencil_dynamic_back.writeMask=255;
 Require(depth_attachment_read_only(&depth));
 // Clear/test requests alone also cannot manufacture an unbound plane.
 for(bool test:{false,true}) for(bool clear:{false,true}) {
  auto absent=ResolveStencilAttachmentUsage(0,test,clear);
  Require(!absent.bound&&!absent.test&&!absent.clear);
  auto present=ResolveStencilAttachmentUsage(1,test,clear);
  Require(present.bound&&present.test==test&&present.clear==clear);
 }
 usage=ResolveStencilAttachmentUsage(1,true,false);
 depth.stencil_test_enable=usage.test;Require(!depth_attachment_read_only(&depth));
 depth.stencil_dynamic_front.writeMask=depth.stencil_dynamic_back.writeMask=0;
 Require(depth_attachment_read_only(&depth));
 depth.stencil_clear_enable=ResolveStencilAttachmentUsage(1,false,true).clear;
 Require(!depth_attachment_read_only(&depth));
 // Unsupported nonzero formats stay bound for the existing format validator.
 Require(ResolveStencilAttachmentUsage(255,true,false).bound);
 std::puts("Captured D32 unbound-stencil state: PASS; bound stencil writes/clears preserved");
}
