#include "guest_safe_memory_copy.h"
#include <array>
#include <cassert>
int main() {
    std::array<unsigned char,18> out;out.fill(0xcc);
    const std::array<unsigned char,8> payload{1,0,3,0,5,0,7,8};
    assert(GuestMemcpyS(out.data()+1,16,payload.data(),8)==0);
    for (int i=0;i<8;++i) assert(out[i+1]==payload[i]);
    assert(out[0]==0xcc && out[9]==0xcc && out[17]==0xcc);
    assert(GuestMemcpyS(out.data()+1,4,payload.data(),8)==34);
    for(int i=1;i<=4;++i) assert(out[i]==0);
    assert(out[5]==5 && out[0]==0xcc);
    assert(GuestMemcpyS(out.data()+1,16,nullptr,8)==22);
    for(int i=1;i<=16;++i) assert(out[i]==0);
    assert(out[0]==0xcc && out[17]==0xcc);
    assert(GuestMemcpyS(nullptr,0,nullptr,0)==0);
    assert(GuestMemcpyS(nullptr,16,payload.data(),8)==22);
    assert(GuestMemcpyS(out.data()+1,16,out.data()+2,8)==22);
    std::array<unsigned char,6> moved{1,2,3,4,5,6};
    assert(GuestMemmoveS(moved.data()+1,5,moved.data(),5)==0);
    assert((moved==std::array<unsigned char,6>{1,1,2,3,4,5}));
    assert(GuestMemmoveS(moved.data(),6,moved.data()+1,5)==0);
    assert((moved==std::array<unsigned char,6>{1,2,3,4,5,5}));
    const auto unchanged=moved;
    assert(GuestMemmoveS(moved.data(),2,moved.data()+1,5)==34 && moved==unchanged);
    assert(GuestMemmoveS(moved.data(),6,nullptr,2)==22 && moved==unchanged);
    assert(GuestMemmoveS(nullptr,0,nullptr,0)==0);

}
