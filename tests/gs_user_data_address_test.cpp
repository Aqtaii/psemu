#include "graphics/guest_gpu/hardwareContext.h"
#include <cstdio>
#include <stdexcept>
using Libs::Graphics::HW::Shader;
void Require(bool v) {if (!v) throw std::runtime_error("GS user data address regression");}
int main() {
 Shader s;
 s.SetGsShaderBase(0x123400);
 s.SetGsUserDataAddress(1,0x1dd);
 s.SetGsUserDataAddress(0,0xaedb5600);
 Require(s.GetVs().gs_regs.user_data_addr==0x1ddaedb5600ull);
 s.SetGsUserDataAddress(0,0xdeadbeef);
 Require(s.GetVs().gs_regs.user_data_addr==0x1dddeadbeefull);
 auto snapshot=s;
 s.SetGsUserDataAddress(1,0x2ee);
 Require(snapshot.GetVs().gs_regs.user_data_addr==0x1dddeadbeefull);
 Require(s.GetVs().gs_regs.user_data_addr==0x2eedeadbeefull);
 Require(s.GetVs().gs_regs.data_addr==0x123400);
 s.Reset(); Require(s.GetVs().gs_regs.user_data_addr==0);
 std::puts("GS address high/low replacement, snapshot isolation and reset passed");
}
