$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\spirvEmitter\spirvEmitterAluOps.cpp")
$begin=$source.IndexOf('void EmitCompareGtU64(')
$end=$source.IndexOf('void EmitCompareNeU64(',$begin)
if($begin -lt 0 -or $end -le $begin){throw 'Production comparison extraction failed'}
$tail=@'
int main() {
 const std::array<uint64_t,12> edges{0,1,0xffffffffull,0x100000000ull,0x100000001ull,0x1ffffffffull,0x200000000ull,0x7fffffffffffffffull,0x8000000000000000ull,0x80000000ffffffffull,0xffffffff00000000ull,UINT64_MAX};
 auto check=[](uint64_t a,uint64_t b) {
  EmitterState state;IR::Instruction inst;inst.src[0].value=a;inst.src[1].value=b;
  EmitCompareGtU64(&state,inst);
  if(state.result!=(a>b))throw std::runtime_error("Unsigned 64-bit emission regression");
 };
 for(auto a:edges)for(auto b:edges)check(a,b);
 uint64_t seed=0x8a743b9125ull;
 for(uint32_t i=0;i<10000;i++) {
  seed=seed*6364136223846793005ull+1;auto a=seed;
  seed=seed*6364136223846793005ull+1;check(a,seed);
 }
 std::puts("PASS: actual SPIR-V U64 comparison boundaries and 10000 input pairs");
}
'@
$file=Join-Path $out 'u64_compare_production.cpp'
[IO.File]::WriteAllText($file,[IO.File]::ReadAllText("$root\tests\u64_compare_emitter_test.cpp")+$source.Substring($begin,$end-$begin)+$tail,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $file /Fe:u64_compare_test.exe
 if($LASTEXITCODE -ne 0){throw 'U64 comparison compilation failed'}
 & .\u64_compare_test.exe
 if($LASTEXITCODE -ne 0){throw 'U64 comparison regression failed'}
} finally {Pop-Location}
