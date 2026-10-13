#include <windows.h> // Match core.h: min/max macros are enabled.
#include "guest_bounded_string.h"
#include <array>
#include <cassert>
#include <limits>
#include <string_view>
int main() {
    std::array<char,258> guarded; guarded.fill('!');
    char* out=guarded.data()+1;
    constexpr auto path="data/prein/effects/odx/hero_respawn_icon_01";
    assert(GuestStrncpyS(out,256,path,23)==0);
    assert(std::string_view(out)=="data/prein/effects/odx/");
    assert(guarded.front()=='!' && guarded.back()=='!' && out[24]=='!');
    assert(GuestStrncpyS(out,5,"abcd",4)==0 && std::string_view(out)=="abcd");
    assert(GuestStrncpyS(out,5,"abcde",5)==34 && out[0]==0);
    assert(GuestStrncpyS(out,5,"xy",100)==0 && std::string_view(out)=="xy");
    assert(GuestStrncpyS(out,5,"abcdef",(std::numeric_limits<std::size_t>::max)())==80);
    assert(std::string_view(out)=="abcd");
    assert(GuestStrncpyS(out,1,"x",(std::numeric_limits<std::size_t>::max)())==80 && out[0]==0);
    assert(GuestStrncpyS(out,1,"",0)==0 && out[0]==0);
    assert(GuestStrncpyS(nullptr,5,"abc",3)==22);
    out[0]='!';assert(GuestStrncpyS(out,0,"abc",3)==22 && out[0]=='!');
}
