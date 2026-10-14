#include "guest_positioned_read.h"
#include <array>
#include <cassert>
#include <cstring>
int main() {
    FILE* file = std::tmpfile(); assert(file);
    const unsigned char payload[] = {10,20,0,40,50,60};
    assert(std::fwrite(payload,1,6,file)==6);
    assert(_fseeki64(file,3,SEEK_SET)==0);
    std::array<unsigned char,8> out; out.fill(0xcc);
    assert(GuestPositionedRead(file,out.data()+1,4,1)==4);
    assert(std::memcmp(out.data()+1,payload+1,4)==0);
    assert(out.front()==0xcc && out[5]==0xcc && _ftelli64(file)==3);
    assert(std::fgetc(file)==40);
    assert(GuestPositionedRead(file,out.data(),8,5)==1 && out[0]==60);
    assert(_ftelli64(file)==4);
    assert(GuestPositionedRead(file,out.data(),8,0x100000000LL)==0);
    assert(_ftelli64(file)==4);
    assert(GuestPositionedRead(file,out.data(),1,-1)==GuestReadInvalid);
    assert(GuestPositionedRead(file,nullptr,1,0)==GuestReadFault);
    assert(GuestPositionedRead(nullptr,out.data(),1,0)==GuestReadBadFd);
    assert(GuestPositionedRead(file,nullptr,0,0)==0 && _ftelli64(file)==4);
    std::fclose(file);
    std::puts("Guest positioned binary reads and cursor preservation passed");
}
