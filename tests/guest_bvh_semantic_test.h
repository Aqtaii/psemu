#pragma once
#include <bit>
#include <cmath>
#include <limits>
struct BvhRecorder {
 using V=std::array<uint64_t,4>;
 uint32_t next=1;
 std::vector<std::vector<uint32_t>> types,code;
 uint32_t AllocateId(){return next++;}
 void AddType(std::initializer_list<uint32_t> x){types.emplace_back(x);}
 void AddFunction(std::initializer_list<uint32_t> x){code.emplace_back(x);}
 void AddFunction(const std::vector<uint32_t>& x){code.push_back(x);}
 void AddAnnotation(std::initializer_list<uint32_t>){}
 void AddName(uint32_t,const char*){}
 static uint32_t Bits(float x){return std::bit_cast<uint32_t>(x);}
 static float Float(uint64_t x){return std::bit_cast<float>(uint32_t(x));}
 static float Half(uint32_t bits) {
  const auto exp=(bits>>10)&31,mant=bits&1023;
  float v=exp==0?std::ldexp(float(mant),-24):
          exp==31?(mant?std::numeric_limits<float>::quiet_NaN():INFINITY):
                  std::ldexp(float(1024+mant),int(exp)-25);
  return bits&0x8000?-v:v;
 }
 V Run(const std::array<V,6>& args,std::unordered_map<uint64_t,uint32_t>& memory,
       const std::function<uint64_t(uint64_t,uint32_t)>& lookup,size_t& reads,
       std::vector<std::pair<uint64_t,uint32_t>>& faults) {
  std::unordered_map<uint32_t,V> v;
  std::unordered_map<uint32_t,size_t> labels;
  for(auto& x:types) {
   if(x[0]==43)v[x[2]]={uint64_t(x[3])|(x.size()==5?uint64_t(x[4])<<32:0),0,0,0};
   if(x[0]==41 || x[0]==42)v[x[2]]={uint64_t(x[0]==41),0,0,0};
   if(x[0]==44){V a{};for(size_t i=3;i<x.size();++i)a[i-3]=v.at(x[i])[0];v[x[2]]=a;}
  }
  unsigned parameter=0;
  for(size_t i=0;i<code.size();++i) {
   auto& x=code[i];if(x[0]==55)v[x[2]]=args.at(parameter++);
   if(x[0]==248)labels[x[1]]=i;
  }
  uint32_t block=0,prev=0;
  for(size_t ip=0;ip<code.size();++ip) {
   auto& x=code[ip];
   auto a=[&](size_t i){return v.at(x[i])[0];};
   auto f=[&](size_t i){return Float(a(i));};
   auto set=[&](uint64_t value){v[x[2]]={value,0,0,0};};
   switch(x[0]) {
    case 54:case 55:case 56:case 247:break;
    case 248:block=x[1];break;
    case 249:prev=block;ip=labels.at(x[1])-1;break;
    case 250:prev=block;ip=labels.at(a(1)?x[2]:x[3])-1;break;
    case 254:return v.at(x[1]);
    case 245:{bool found=false;for(size_t i=3;i<x.size();i+=2)
      if(x[i+1]==prev){v[x[2]]=v.at(x[i]);found=true;break;}
      if(!found)throw std::runtime_error("BVH phi predecessor mismatch");break;}
    case 57:if(x[3]==100010) {
      auto p=v.at(x[4]);auto guest=p[0]|(p[1]<<32);
      auto device=lookup(guest,uint32_t(a(5)));v[x[2]]={uint32_t(device),device>>32,0,0};
     } else if(x[3]==100011) {
      auto p=v.at(x[4]);faults.emplace_back(p[0]|(p[1]<<32),uint32_t(a(5)));set(a(5));
     } else throw std::runtime_error("unexpected BVH external function");break;
    case 12:{auto op=x[4];auto fa=Float(a(5));
      if(op==4)set(Bits(std::fabs(fa)));
      else if(op==37)set(Bits(std::fmin(fa,Float(a(6)))));
      else if(op==40)set(Bits(std::fmax(fa,Float(a(6)))));
      else if(op==38)set(std::min(uint32_t(a(5)),uint32_t(a(6))));
      else if(op==62)v[x[2]]={Bits(Half(uint32_t(a(5))&65535)),Bits(Half(uint32_t(a(5))>>16)),0,0};
      else throw std::runtime_error("unexpected BVH GLSL instruction");break;}
    case 80:{V result{};for(size_t i=3;i<x.size();++i)result[i-3]=a(i);v[x[2]]=result;break;}
    case 81:set(v.at(x[3]).at(x[4]));break;
    case 124:v[x[2]]=v.at(x[3]);break;
    case 113:set(x[1]==100000?uint32_t(a(3)):a(3));break;
    case 120:set(a(3));break;
    case 61:if(x.size()!=6 || x[4]!=2 || x[5]!=4)throw std::runtime_error("unaligned BVH read");
      set(memory.at(a(3)));++reads;break;
    case 128:set(x[1]==100003?a(3)+a(4):uint32_t(a(3)+a(4)));break;
    case 132:set(uint32_t(a(3)*a(4)));break;
    case 127:set(Bits(-f(3)));break;
    case 129:set(Bits(f(3)+f(4)));break;
    case 131:set(Bits(f(3)-f(4)));break;
    case 133:set(Bits(f(3)*f(4)));break;
    case 156:set(std::isnan(f(3)));break;
    case 157:set(std::isinf(f(3)));break;
    case 165:set(bool(a(3))!=bool(a(4)));break;
    case 166:set(bool(a(3))||bool(a(4)));break;
    case 167:set(bool(a(3))&&bool(a(4)));break;
    case 168:set(!a(3));break;
    case 169:v[x[2]]=v.at(a(3)?x[4]:x[5]);break;
    case 170:set(a(3)==a(4));break;
    case 171:set(a(3)!=a(4));break;
    case 172:set(a(3)>a(4));break;
    case 176:set(a(3)<a(4));break;
    case 178:set(a(3)<=a(4));break;
    case 180:set(f(3)==f(4));break;
    case 184:set(f(3)<f(4));break;
    case 186:set(f(3)>f(4));break;
    case 188:set(f(3)<=f(4));break;
    case 190:set(f(3)>=f(4));break;
    case 194:set(a(3)>>a(4));break;
    case 196:set(x[1]==100003?a(3)<<a(4):uint32_t(a(3)<<a(4)));break;
    case 197:set(a(3)|a(4));break;
    case 199:set(a(3)&a(4));break;
    default:throw std::runtime_error("unexpected BVH SPIR-V instruction "+std::to_string(x[0]));
   }
  }
  throw std::runtime_error("BVH function has no result");
 }
};
inline void TestGuestBvhSemantics() {
 BvhRecorder code;
 Bvh::Define(code,100000,100001,100002,100003,100004,100005,100006,100007,
             100008,100009,100010,100011);
 using V=BvhRecorder::V;
 const uint64_t base=0x26500000000ull,first=0x800000000ull,second=first+65536;
 std::unordered_map<uint64_t,uint32_t> memory;
 auto put=[&](unsigned word,uint32_t value){memory[(word<16?first:second)+(word%16)*4]=value;};
 auto float_word=[&](unsigned word,float value){put(word,BvhRecorder::Bits(value));};
 std::array<V,6> args{
  V{uint32_t(base>>8),uint32_t((base>>40)&255),1,0x80000000},
  V{0,0,0,0}, V{BvhRecorder::Bits(100),0,0,0},
  V{BvhRecorder::Bits(.25f),BvhRecorder::Bits(.25f),0,0},
  V{0,0,BvhRecorder::Bits(1),0},
  V{BvhRecorder::Bits(INFINITY),BvhRecorder::Bits(INFINITY),BvhRecorder::Bits(1),0}};
 auto lookup=[&](uint64_t guest,uint32_t bytes) {
  if(bytes!=64)throw std::runtime_error("BVH did not resolve complete block");
  return guest==base?first:guest==base+64?second:0ull;
 };
 size_t reads=0;std::vector<std::pair<uint64_t,uint32_t>> faults;
 // Triangle0: origin lies inside a triangle one unit along +Z.
 const std::array<float,15> positions{0,0,1, 1,0,1, 0,1,1, 1,1,1, 2,1,1};
 for(unsigned i=0;i<15;++i)float_word(i,positions[i]);put(15,0x12345600);
 auto hit=code.Run(args,memory,lookup,reads,faults);
 if(BvhRecorder::Float(hit[0])/BvhRecorder::Float(hit[1])!=1 || hit[2]!=0x12345600 || hit[3]!=1 || !faults.empty())
  throw std::runtime_error("BVH triangle hit mismatch");
 args[3][0]=BvhRecorder::Bits(3);args[3][1]=BvhRecorder::Bits(3);
 auto miss=code.Run(args,memory,lookup,reads,faults);
 if(miss[0]!=0x7f800000 || miss[1]!=0x3f800000 || miss[3]!=0)
  throw std::runtime_error("BVH triangle miss mismatch");
 auto invalid=args;invalid[0][3]=0;auto before=reads;
 if(code.Run(invalid,memory,lookup,reads,faults)!=V{0x7f800000,0x3f800000,UINT32_MAX,0} || reads!=before)
  throw std::runtime_error("invalid BVH descriptor touched memory");
 invalid=args;invalid[3][0]=BvhRecorder::Bits(NAN);
 code.Run(invalid,memory,lookup,reads,faults);if(reads!=before)
  throw std::runtime_error("NaN BVH ray touched memory");
 auto absent=[](uint64_t,uint32_t){return uint64_t{0};};
 code.Run(args,memory,absent,reads,faults);
 if(reads!=before || faults.size()!=1 || faults[0]!=std::pair<uint64_t,uint32_t>{base,1})
  throw std::runtime_error("missing BVH block was not diagnosed");
 faults.clear();
 // Full boxes span two independently backed 64-byte blocks.
 args[1][0]=5;args[3]={0,0,0,0};
 args[4]={BvhRecorder::Bits(1),BvhRecorder::Bits(1),BvhRecorder::Bits(1),0};
 args[5]=args[4];
 for(unsigned child=0;child<4;++child) {
  put(child,10+child);
  for(unsigned axis=0;axis<3;++axis) {
   float_word(4+child*6+axis,float(4-child));
   float_word(4+child*6+3+axis,float(5-child));
  }
 }
 auto unsorted=code.Run(args,memory,lookup,reads,faults);
 if(unsorted!=V{10,11,12,13})throw std::runtime_error("full boxes discontiguous read mismatch");
 args[0][1]|=0x80000000;
 if(code.Run(args,memory,lookup,reads,faults)!=V{13,12,11,10})
  throw std::runtime_error("BVH box sorting mismatch");
 auto no_second=[&](uint64_t guest,uint32_t bytes){return guest==base?lookup(guest,bytes):uint64_t{0};};
 before=reads;faults.clear();
 if(code.Run(args,memory,no_second,reads,faults)!=V{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} ||
    reads!=before || faults.size()!=1 || faults[0].first!=base+64)
  throw std::runtime_error("missing second BVH block was accessed");
 // Half boxes use one block, unpacking six half floats per child.
 args[1][0]=4;
 for(unsigned child=0;child<4;++child)for(unsigned pair=0;pair<3;++pair)
  put(4+child*3+pair,std::array<uint32_t,3>{0x3c003c00,0x40003c00,0x40004000}[pair]);
 auto halves=code.Run(args,memory,lookup,reads,faults);
 if(halves!=V{10,11,12,13})throw std::runtime_error("half boxes mismatch");
 std::puts("PASS: production BVH triangle hit/miss, invalid and NaN rays, full/half boxes, sorting, discontiguous backing and missing blocks");
}
