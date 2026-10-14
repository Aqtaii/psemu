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
    assert(GuestStrcpyS(out,256,"hero_respawn_icon_01")==0);
    assert(std::string_view(out)=="hero_respawn_icon_01");
    assert(GuestStrcpyS(out,4,"long-name")==34 && out[0]==0);
    assert(GuestStrncpyS(out,256,path,19)==0);
    const auto suffix=std::string_view("skel/hero_respawn_icon_01.skel");
    assert(GuestWriteFormattedString(out+19,256-19,suffix)==static_cast<int>(suffix.size()));
    assert(std::string_view(out)=="data/prein/effects/skel/hero_respawn_icon_01.skel");
    assert(guarded.front()=='!' && guarded.back()=='!');
    assert(GuestWriteFormattedString(out,4,"long-name")==-1 && out[0]==0);
    assert(GuestWriteFormattedString(out,1,"")==0 && out[0]==0);
    out[0]='!';assert(GuestWriteFormattedString(out,0,"abc")==-1 && out[0]=='!');

    assert(GuestStrcpyS(out,256,"data/prein/effects/")==0);
    assert(GuestStrncatS(out,256,"skel/",5)==0);
    assert(GuestStrncatS(out,256,"hero_respawn_icon_01",20)==0);
    assert(GuestStrncatS(out,256,".skel",5)==0);
    assert(std::string_view(out)=="data/prein/effects/skel/hero_respawn_icon_01.skel");
    assert(guarded.front()=='!' && guarded.back()=='!');
    assert(GuestStrncatS(out,256,"ignored",0)==0);
    assert(std::string_view(out)=="data/prein/effects/skel/hero_respawn_icon_01.skel");
    assert(GuestStrcpyS(out,256,"abc")==0);
    assert(GuestStrncatS(out,5,"de",2)==34 && out[0]==0);
    assert(GuestStrcpyS(out,256,"abc")==0);
    assert(GuestStrncatS(out,5,"def",SIZE_MAX)==80 && std::string_view(out)=="abcd");
    std::memset(out,'x',4);
    assert(GuestStrncatS(out,4,"y",1)==22 && out[0]==0);
    assert(GuestStrncatS(nullptr,4,"y",1)==22);
}
