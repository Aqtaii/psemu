import struct,re,bisect,sys,json,argparse
from pathlib import Path
from capstone import Cs,CS_ARCH_X86,CS_MODE_64,CS_GRP_JUMP,CS_GRP_RET,CS_GRP_IRET
from capstone.x86_const import X86_OP_IMM,X86_INS_JMP,X86_INS_LJMP
parser=argparse.ArgumentParser(description='Generate verified TLS sites for the owned PPSA21564 1.007 plaintext SELF. Requires capstone.')
parser.add_argument('self_file',type=Path);parser.add_argument('output',type=Path);parser.add_argument('--check',action='store_true')
args=parser.parse_args();p=args.self_file
with p.open('rb') as f:
 f.seek(0x3b1f0);code=f.read(0x74edc9c)
 f.seek(0x7532df0);hdr=f.read(0x262bd4)
if hdr[:4]!=b'\x01\x1b\x03\x3b' or len(code)!=0x74edc9c:raise ValueError('Unsupported SELF layout')
n=struct.unpack_from('<I',hdr,8)[0]
if n!=312697:raise ValueError('Unsupported unwind table; use this tool only on the pinned game build')
starts=[0x74f0000+struct.unpack_from('<i',hdr,12+i*8)[0] for i in range(n)]
assert starts==sorted(starts)
pattern=re.compile(rb'\x64[\x48\x4c]\x8b[\x04\x0c\x14\x1c\x24\x2c\x34\x3c]\x25\x00\x00\x00\x00')
candidates=[m.start() for m in pattern.finditer(code)];functions={bisect.bisect_right(starts,x)-1 for x in candidates};cs=Cs(CS_ARCH_X86,CS_MODE_64);cs.detail=True
sites=[]
for i in functions:
 start=starts[i];end=starts[i+1] if i+1<len(starts) else len(code)
 if not 0<=start<end<=len(code):continue
 queue=[start];seen=set()
 while queue:
  pc=queue.pop()
  if pc in seen or not start<=pc<end:continue
  for instruction in cs.disasm(code[pc:end],pc):
   addr,size,mn=instruction.address,instruction.size,instruction.mnemonic
   if addr in seen:break
   seen.add(addr);raw=code[addr:addr+size]
   if pattern.fullmatch(raw.lstrip(b'\x66')):sites.append({'rva':addr,'bytes':raw.hex(),'context':code[addr-8:addr+size+8].hex()})
   if instruction.group(CS_GRP_JUMP):
    if instruction.operands and instruction.operands[0].type==X86_OP_IMM:
     target=instruction.operands[0].imm
     if start<=target<end:queue.append(target)
    if instruction.id in (X86_INS_JMP,X86_INS_LJMP):break
   if instruction.group(CS_GRP_RET) or instruction.group(CS_GRP_IRET) or mn in ('int3','ud2','hlt'):break
sites.sort(key=lambda x:x['rva'])
assert len(sites)==len({x['rva'] for x in sites})
header="""#pragma once
#include <array>
#include <cstdint>
#include <cstring>
// Astro PPSA21564: sites reached from real .eh_frame_hdr function starts by
// control-flow decoding. Original instruction and both adjacent contexts must
// match before entry; unmatched sites retain VEH. No mutation after guest entry.
struct AstroTlsSite { uint32_t rva; uint64_t before; uint64_t after; };
inline constexpr std::array<uint8_t,12> AstroTlsInstruction={0x66,0x66,0x66,0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
inline constexpr AstroTlsSite AstroTlsSites[]={
"""
for site in sites:
 raw=bytes.fromhex(site['bytes']);context=bytes.fromhex(site['context'])
 if raw!=bytes.fromhex('66666664488b042500000000'):raise ValueError('Unsupported TLS opcode')
 header+=f" {{0x{site['rva']:x},0x{int.from_bytes(context[:8],'little'):016x}ULL,0x{int.from_bytes(context[-8:],'little'):016x}ULL}},\n"
header+="""};
inline bool PrepareAstroTlsPatch(const uint8_t* context,const AstroTlsSite& site,uint32_t teb_slot,std::array<uint8_t,12>* patch) {
 if(!context || !patch || !teb_slot || teb_slot>0xffff)return false;
 if(std::memcmp(context,&site.before,8) || std::memcmp(context+8,AstroTlsInstruction.data(),12) ||
    std::memcmp(context+20,&site.after,8))return false;
 *patch=AstroTlsInstruction;(*patch)[3]=0x65;std::memcpy(patch->data()+8,&teb_slot,4);return true;
}
"""
if args.check:
 if args.output.read_text(encoding='utf-8')!=header:raise ValueError('Generated header differs')
else:args.output.write_text(header,encoding='utf-8',newline='\n')
print(f"PASS: {len(candidates)} candidates, {len(sites)} CFG-verified exact TLS instructions")
