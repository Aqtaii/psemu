$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
$s=[IO.File]::ReadAllText("$root\src\core.cpp")
$a=$s.IndexOf('static PSEMU_SYSV int NativeIsFiniteFloat(');$b=$s.IndexOf('static PSEMU_SYSV double NativeSin(',$a)
if($a -lt 0 -or $b -le $a){throw 'Production float classifiers missing'}
$native=$s.Substring($a,$b-$a)
$a=$s.IndexOf('} else if (readable_name == "__isfinitef"');$b=$s.IndexOf('} else if (readable_name == "fp_isfinite")',$a)
if($a -lt 0 -or $b -le $a){throw 'Production float fallback missing'}
$fallback=$s.Substring($a,$b-$a)
$prefix=@"
#define NOMINMAX
#include <windows.h>
#include <immintrin.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include "$root/include/guest_float_classify.h"
#define PSEMU_SYSV __attribute__((sysv_abi))
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
"@
$wrap=@"
static uint64_t Fallback(const std::string& readable_name,uint64_t poisoned_xmm) {
 CONTEXT state{};state.Xmm0.Low=poisoned_xmm;CONTEXT* ctx=&state;bool special_return_set=false;
 if(false){
"@
$after=@"
 }return special_return_set?ctx->Rax:99;
}
int main(){
 using Classify=int (__attribute__((sysv_abi)) *)(float);
 const Classify functions[]={&NativeIsFiniteFloat,&NativeIsNaNFloat,&NativeIsInfFloat};
 const char* names[]={"__isfinitef","__isnanf","__isinff"};
 const uint32_t values[]={0,0x80000000,1,0x80000001,0x007fffff,0x00800000,0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,0xffc12345,0x7f800001,0xff800001};
 for(uint32_t bits:values){float value;std::memcpy(&value,&bits,4);
  const unsigned before=_mm_getcsr();int actual[3]{};
  for(int j=0;j<3;++j)actual[j]=functions[j](value);
  CHECK(_mm_getcsr()==before);
  const int expected[]={std::isfinite(value)?1:0,std::isnan(value)?1:0,std::isinf(value)?1:0};
  for(int j=0;j<3;++j){CHECK(actual[j]==expected[j]);CHECK(Fallback(names[j],0xdeadbeef00000000ull|bits)==uint64_t(expected[j]));}
  _mm_setcsr(before);
 }
 CHECK(NativeIsFiniteFloat(0.1f)==1 && Fallback("__isfinitef",0x3dcccccdu)==1);
 std::puts("[FLOAT-CLASSIFY-TEST] real SysV float calls and actual VEH branch: finite/zero/subnormal/NaN/sNaN/signed INF, poisoned upper XMM bits and unchanged MXCSR passed");
}
"@
New-Item -ItemType Directory -Path $out -Force | Out-Null
[IO.File]::WriteAllText("$out\guest_float_classify_production.cpp",$prefix+"`n"+$native+"`n"+$wrap+"`n"+$fallback+"`n"+$after,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /Od /EHsc /MD guest_float_classify_production.cpp /Fe:guest_float_classify_test.exe
 if($LASTEXITCODE){throw 'Float classification compilation failed'}
 & .\guest_float_classify_test.exe
 if($LASTEXITCODE){throw 'Float classification regression failed'}
} finally {Pop-Location}
