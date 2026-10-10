$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\spirvEmitter\spirvEmitterAluOps.cpp")
$begin=$source.IndexOf('void EmitCompareGtU64(')
$end=$source.IndexOf('void EmitCompareNeU64(',$begin)
if($begin -lt 0 -or $end -le $begin){throw 'Production comparison extraction failed'}
$borrowBegin=$source.IndexOf('void EmitISubBorrowU32(')
$borrowEnd=$source.IndexOf('void EmitScalarAddCarryU32(',$borrowBegin)
if($borrowBegin -lt 0 -or $borrowEnd -le $borrowBegin){throw 'Production borrow extraction failed'}
$borrow=$source.Substring($borrowBegin,$borrowEnd-$borrowBegin)
$ir=[IO.File]::ReadAllText("$root\gpu\src\graphics\shader\recompiler\shaderIR\ShaderIR.cpp")
$lowerBegin=$ir.IndexOf('bool LowerVectorAddCarry(')
$lowerEnd=$ir.IndexOf('bool LowerVectorCarryOut(',$lowerBegin)
if($lowerBegin -lt 0 -or $lowerEnd -le $lowerBegin){throw 'Production lowering extraction failed'}
$lower=$ir.Substring($lowerBegin,$lowerEnd-$lowerBegin)
$tail=@'
int main() {
 for(auto op:{Decoder::Opcode::VAddcU32,Decoder::Opcode::VSubCoCiU32,Decoder::Opcode::VSubrevCoCiU32}) {
  Decoder::Instruction decoded;decoded.opcode=op;decoded.pc=0xb84;
  decoded.src0.value=17;decoded.src1.value=23;decoded.src2.value=0x8000000000000000ull;
  BasicBlock block;std::string error;
  if(!LowerVectorAddCarry(decoded,&block,&error) || block.instructions.size()!=1)throw std::runtime_error("Lowering failed");
  const auto& lowered=block.instructions[0];
  const bool reverse=op==Decoder::Opcode::VSubrevCoCiU32;
  if(lowered.src[0].value!=(reverse?23:17) || lowered.src[1].value!=(reverse?17:23) || lowered.src_count!=3 ||
     lowered.src[2].value!=decoded.src2.value || lowered.op!=(op==Decoder::Opcode::VAddcU32?Opcode::IAddCarryU32:Opcode::ISubBorrowU32))
    throw std::runtime_error("Subtraction operand order or borrow mask lost");
 }
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
 for(uint32_t lane: {0u,31u,32u,63u}) {
  for(uint32_t a: {0u,1u,2u,0x7fffffffu,0x80000000u,0xfffffffeu,UINT32_MAX}) {
   for(uint32_t b: {0u,1u,2u,0x7fffffffu,0x80000000u,0xfffffffeu,UINT32_MAX}) {
    for(uint32_t incoming: {0u,1u}) {
     EmitterState state;state.lane=lane;IR::Instruction inst;inst.src_count=3;
     inst.src[0].value=a;inst.src[1].value=b;inst.src[2].value=uint64_t(incoming)<<lane;
     EmitISubBorrowU32(&state,inst);
     const auto subtrahend=uint64_t(b)+incoming;
     if(state.subtraction!=static_cast<uint32_t>(uint64_t(a)-subtrahend) || state.borrow!=(uint64_t(a)<subtrahend))
      throw std::runtime_error("Vector borrow-in or borrow-out regression");
     inst.src_count=2;EmitterState plain;EmitISubBorrowU32(&plain,inst);
     if(plain.subtraction!=a-b || plain.borrow!=(a<b))throw std::runtime_error("Existing borrow regression");
    }
   }
  }
 }
 std::puts("PASS: actual U64 comparison and vector borrow across lane and arithmetic boundaries");
}
'@
$file=Join-Path $out 'u64_compare_production.cpp'
[IO.File]::WriteAllText($file,[IO.File]::ReadAllText("$root\tests\u64_compare_emitter_test.cpp")+$source.Substring($begin,$end-$begin)+$borrow+$lower+$tail,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD $file /Fe:u64_compare_test.exe
 if($LASTEXITCODE -ne 0){throw 'U64 comparison compilation failed'}
 & .\u64_compare_test.exe
 if($LASTEXITCODE -ne 0){throw 'U64 comparison regression failed'}
} finally {Pop-Location}
