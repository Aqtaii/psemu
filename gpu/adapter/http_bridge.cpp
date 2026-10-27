#include "libs/network.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include <cstdlib>
#include <mutex>

// Reuse the existing Kyty offline HTTP lifecycle; no socket/PSN imports are registered.
namespace {
std::once_flag http_init;
void InitState() {
    std::call_once(http_init, [] { Libs::Network::NetworkSubsystem::Instance()->Init(nullptr); });
}
}
namespace Libs::PsemuHttp {
LIB_VERSION("Http",1,"Http",1,1);
LIB_DEFINE(Register) {
    LIB_FUNC("A9cVMUtEp4Y", Network::Http::HttpInit);
    LIB_FUNC("Ik-KpLTlf7Q", Network::Http::HttpTerm);
    LIB_FUNC("0gYjPTR-6cY", Network::Http::HttpCreateTemplate);
    LIB_FUNC("4I8vEpuEhZ8", Network::Http::HttpDeleteTemplate);
    LIB_FUNC("s2-NPIvz+iA", Network::Http::HttpSetNonblock);
    LIB_FUNC("htyBOoWeS58", Network::Http::HttpsSetSslCallback);
    LIB_FUNC("jUjp+yqMNdQ", Network::Http::HttpsSetMinSslVersion);
    LIB_FUNC("mSQCxzWTwVI", Network::Http::HttpsDisableOption);
    LIB_FUNC("6381dWF+xsQ", Network::Http::HttpCreateEpoll);
    LIB_FUNC("wYhXVfS2Et4", Network::Http::HttpDestroyEpoll);
    LIB_FUNC("-xm7kZQNpHI", Network::Http::HttpSetEpoll);
    LIB_FUNC("59tL1AQBb8U", Network::Http::HttpUnsetEpoll);
    LIB_FUNC("Kiwv9r4IZCc", Network::Http::HttpCreateConnection);
    LIB_FUNC("qgxDBjorUxs", Network::Http::HttpCreateConnectionWithURL);
    LIB_FUNC("P6A3ytpsiYc", Network::Http::HttpDeleteConnection);
    LIB_FUNC("tsGVru3hCe8", Network::Http::HttpCreateRequest);
    LIB_FUNC("Aeu5wVKkF9w", Network::Http::HttpCreateRequest);
    LIB_FUNC("Cnp77podkCU", Network::Http::HttpCreateRequestWithURL2);
    LIB_FUNC("qe7oZ+v4PWA", Network::Http::HttpDeleteRequest);
    LIB_FUNC("PTiFIUxCpJc", Network::Http::HttpSetRequestContentLength);
    LIB_FUNC("EY28T2bkN7k", Network::Http::HttpAddRequestHeader);
    LIB_FUNC("1e2BNwI-XzE", Network::Http::HttpSendRequest);
    LIB_FUNC("hvG6GfBMXg8", Network::Http::HttpAbortRequest);
    LIB_FUNC("qISjDHrxONc", Network::Http::HttpWaitRequest);
    LIB_FUNC("0a2TBNfE3BU", Network::Http::HttpGetStatusCode);
    LIB_FUNC("aCYPMSUIaP8", Network::Http::HttpGetAllResponseHeaders);
    LIB_FUNC("yuO2H2Uvnos", Network::Http::HttpGetResponseContentLength);
    LIB_FUNC("Tc-hAYDKtQc", Network::Http::HttpSetResolveTimeOut);
    LIB_FUNC("K1d1LqZRQHQ", Network::Http::HttpSetResolveRetry);
    LIB_FUNC("0S9tTH0uqTU", Network::Http::HttpSetConnectTimeOut);
    LIB_FUNC("xegFfZKBVlw", Network::Http::HttpSetSendTimeOut);
    LIB_FUNC("yigr4V0-HTM", Network::Http::HttpSetRecvTimeOut);
    LIB_FUNC("T-mGo9f3Pu4", Network::Http::HttpSetAutoRedirect);
    LIB_FUNC("qFg2SuyTJJY", Network::Http::HttpSetAuthEnabled);
}
}
namespace Libs::PsemuSsl {
LIB_VERSION("Ssl",1,"Ssl",1,1);
LIB_DEFINE(Register) {
    LIB_FUNC("hdpVEUDFW3s",Network::Ssl::SslInit);
    LIB_FUNC("0K1yQ6Lv-Yc",Network::Ssl::SslTerm);
    LIB_FUNC("TDfQqO-gMbY",Network::Ssl::SslGetCaCerts);
    LIB_FUNC("qIvLs0gYxi0",Network::Ssl::SslFreeCaCerts);
}
}
namespace Libs::PsemuNetPool {
LIB_VERSION("Net",1,"Net",1,1);
LIB_DEFINE(Register) {
    LIB_FUNC("dgJBaeJnGpo",Network::Net::NetPoolCreate);
    LIB_FUNC("K7RlrTkI-mw",Network::Net::NetPoolDestroy);
}
}
extern "C" void PsemuRegisterOfflineHttp(Loader::SymbolDatabase* db) {
    const char* enabled=std::getenv("PSEMU_NATIVE_HTTP");
    if(!enabled || enabled[0]!='1')return;
    InitState();
    Libs::PsemuNetPool::Register(db);
    Libs::PsemuSsl::Register(db);
    Libs::PsemuHttp::Register(db);
}
