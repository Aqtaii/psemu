$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\spirvEmitter\spirvEmitterAluOps.cpp")
$begin=$source.IndexOf('void EmitFindLsbU64(')
$end=$source.IndexOf('void EmitFindMsbFromHighU32(',$begin)
if($begin -lt 0 -or $end -le $begin){throw 'FF1 U64 production extraction failed'}
$tail=@'
int main() {
 auto check=[](uint64_t value) {
  EmitterState state;IR::Instruction inst;inst.src[0].value=value;
  EmitFindLsbU64(&state,inst);
  const auto expected=value?static_cast<uint32_t>(std::countr_zero(value)):UINT32_MAX;
  if(state.result!=expected || state.reads!=2 || state.writes!=1)throw std::runtime_error("FF1 U64 production regression");
 };
 check(0);check(UINT64_MAX);check(0x0000010000000000ull);check(0x100000020ull);
 for(uint32_t bit=0;bit<64;++bit){check(1ull<<bit);check((1ull<<bit)|0x8000000000000000ull);}
 uint64_t seed=0x4567849831ull;
 for(uint32_t i=0;i<10000;++i){seed=seed*6364136223846793005ull+1;check(seed);}
 std::puts("PASS: production FF1 U64 across zero, all bit positions, overlap ordering and 10000 inputs");
}
'@
$file=Join-Path $out 'ff1_u64_production.cpp'
[IO.File]::WriteAllText($file,[IO.File]::ReadAllText("$root\tests\ff1_u64_emitter_test.cpp")+$source.Substring($begin,$end-$begin)+$tail,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $file /Fe:ff1_u64_test.exe
 if($LASTEXITCODE -ne 0){throw 'FF1 U64 compilation failed'}
 & .\ff1_u64_test.exe
 if($LASTEXITCODE -ne 0){throw 'FF1 U64 regression failed'}
} finally {Pop-Location}
