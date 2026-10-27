#include "libs/network.h"
#include "libs/errno.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
extern "C" void* PsemuKytyGetFunc(const char* nid);
#define CHECK(x) do {if(!(x)){std::printf("[HTTP-TEST] FAIL line=%d: %s\n",__LINE__,#x);return 1;}}while(0)
extern "C" int PsemuHttpSelfTest() {
    const char* old=std::getenv("PSEMU_NATIVE_HTTP");
    struct Restore { std::string value; ~Restore(){_putenv_s("PSEMU_NATIVE_HTTP",value.c_str());} } restore{old?old:""};
    _putenv_s("PSEMU_NATIVE_HTTP","1");
    using namespace Libs::Network;
    const auto pool_fn=reinterpret_cast<decltype(&Net::NetPoolCreate)>(PsemuKytyGetFunc("dgJBaeJnGpo"));
    const auto status_fn=reinterpret_cast<decltype(&Http::HttpGetStatusCode)>(PsemuKytyGetFunc("0a2TBNfE3BU"));
    const auto length_fn=reinterpret_cast<decltype(&Http::HttpGetResponseContentLength)>(PsemuKytyGetFunc("yuO2H2Uvnos"));
    const auto send_fn=reinterpret_cast<decltype(&Http::HttpSendRequest)>(PsemuKytyGetFunc("1e2BNwI-XzE"));
    CHECK(pool_fn==&Net::NetPoolCreate && status_fn==&Http::HttpGetStatusCode &&
          length_fn==&Http::HttpGetResponseContentLength && send_fn==&Http::HttpSendRequest);
    const int pool=pool_fn("psemu-offline-http",65536,0);CHECK(pool>=0);
    const int ssl=Ssl::SslInit(65536);CHECK(ssl>0);
    const int http=Http::HttpInit(pool,ssl,65536);CHECK(http>0);
    const int tmpl=Http::HttpCreateTemplate(http,"psemu-test",1,0);CHECK(tmpl>0);
    const int connection=Http::HttpCreateConnectionWithURL(tmpl,"https://psemu.invalid/",0);CHECK(connection>0);
    const int request=Http::HttpCreateRequestWithURL2(connection,"GET","https://psemu.invalid/test",0);CHECK(request>0);
    struct Status { uint32_t before; int value; uint32_t after; } status{0xaabbccdd,123,0x11223344};
    CHECK(status_fn(request,&status.value)==HTTP_ERROR_BEFORE_SEND && status.value==0);
    CHECK(status.before==0xaabbccdd && status.after==0x11223344);
    Http::HttpEpollHandle epoll=nullptr;CHECK(Http::HttpCreateEpoll(http,&epoll)==0 && epoll);
    CHECK(Http::HttpSetNonblock(request,1)==0 && Http::HttpSetEpoll(request,epoll,nullptr)==0);
    CHECK(send_fn(request,nullptr,0)==HTTP_ERROR_TIMEOUT);
    status.value=123;CHECK(status_fn(request,&status.value)==HTTP_ERROR_TIMEOUT && status.value==0);
    struct Length {uint64_t before,value,after;} length{0x1122334455667788ULL,~0ULL,0x8877665544332211ULL};
    int result=123;CHECK(length_fn(request,&result,&length.value)==HTTP_ERROR_TIMEOUT && result==-1 && length.value==0);
    CHECK(length.before==0x1122334455667788ULL && length.after==0x8877665544332211ULL);
    alignas(8) std::array<uint8_t,24> events{};events.fill(0xa5);const auto untouched=events;
    CHECK(Http::HttpWaitRequest(epoll,reinterpret_cast<Http::HttpNBEvent*>(events.data()),1,0)==0 && events==untouched);
    CHECK(status_fn(request,nullptr)==HTTP_ERROR_INVALID_VALUE);
    CHECK(Http::HttpUnsetEpoll(request)==0 && Http::HttpDeleteRequest(request)==0);
    status.value=123;CHECK(status_fn(request,&status.value)==HTTP_ERROR_INVALID_ID && status.value==0);
    result=123;length.value=~0ULL;CHECK(length_fn(request,&result,&length.value)==HTTP_ERROR_INVALID_ID && result==0 && length.value==0);
    CHECK(Http::HttpDestroyEpoll(http,epoll)==0 && Http::HttpDeleteConnection(connection)==0 && Http::HttpDeleteTemplate(tmpl)==0);
    CHECK(Http::HttpTerm(http)==0 && Ssl::SslTerm(ssl)==0 && Net::NetPoolDestroy(pool)==0);
    std::puts("[HTTP-TEST] real NID dispatch, typed IDs, before-send/timeout/deleted errors, exact zero outputs/canaries, empty poll; no network request or game frame");
    return 0;
}
