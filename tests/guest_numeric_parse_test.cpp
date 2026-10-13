#include "guest_numeric_parse.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
static void Require(bool ok){if(!ok)std::abort();}
int main(){
 uint32_t output=0xfeedface;
 Require(GuestScanHex32("  0x2A",&output)==1 && output==42);
 Require(GuestScanHex32("FFFFFFFF",&output)==1 && output==0xffffffffu);
 Require(GuestScanHex32("-1",&output)==1 && output==0xffffffffu);
 Require(GuestScanHex32("12tail",&output)==1 && output==0x12u);
 output=0xfeedface;
 Require(GuestScanHex32("invalid",&output)==0 && output==0xfeedface);
 Require(GuestScanHex32("",&output)==EOF && output==0xfeedface);
 Require(GuestScanHex32("   ",&output)==EOF && output==0xfeedface);
 int32_t decimal=-999;
 Require(GuestScanDecimal32(" -42suffix",&decimal)==1 && decimal==-42);
 Require(GuestScanDecimal32("2147483647",&decimal)==1 && decimal==2147483647);
 Require(GuestScanDecimal32("-2147483648",&decimal)==1 && decimal==(-2147483647-1));
 decimal=-999;
 Require(GuestScanDecimal32("invalid",&decimal)==0 && decimal==-999);
 Require(GuestScanDecimal32("",&decimal)==EOF && decimal==-999);
 char* end=nullptr;
 const char* large="4294967297suffix";
 Require(GuestUnsignedInteger(large,&end,10)==4294967297ULL && end==large+10);
 const char* hex="0xf10045fe";
 Require(GuestUnsignedInteger(hex,&end,0)==0xf10045feULL && *end=='\0');
 const char* invalid="invalid";
 Require(GuestUnsignedInteger(invalid,&end,10)==0 && end==invalid);
 Require(GuestDecimalInteger(" +123suffix")==123);
 Require(GuestDecimalInteger("-2147483648")==-2147483648LL);
 Require(GuestDecimalInteger("4294967297")==4294967297LL);
 Require(GuestDecimalInteger("9223372036854775807")==std::numeric_limits<int64_t>::max());
 Require(GuestDecimalFloat(" -1.25e2suffix")==-125.0);
 Require(GuestDecimalFloat("0.125")==0.125);
 Require(GuestDecimalFloat("invalid")==0.0);
 Require(std::isnan(GuestDecimalFloat("nan")));
 std::puts("[GUEST-NUMERIC] hex assignment/failure/EOF and decimal integer/float passed");
}
