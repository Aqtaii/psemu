#include "../include/guest_directory.h"
#include <cassert>
#include <array>
#include <set>
int main(){
 std::vector<GuestDirectoryEntry> entries{{"astro.spx",true,17},{"rooms",false,18}};
 for(int i=0;i<80;++i)entries.push_back({"script_"+std::to_string(i)+".spx",true,uint32_t(i+19)});
 entries.push_back({std::string(255,'x'),true,99});
 GuestDirectorySnapshot directory;assert(GuestPackDirectory(entries,&directory.bytes));
 assert(directory.bytes.size()%512==0 && directory.bytes.size()>512);
 std::set<std::string> names;std::array<uint8_t,514> block;int64_t base=0;size_t offset=0;
 for(;;) {
  block.fill(0xa5);auto count=GuestReadDirectory(directory,block.data()+1,512,&base);
  assert(base==int64_t(offset) && block.front()==0xa5 && block.back()==0xa5);
  if(!count)break;assert(count==512);offset+=512;
  for(size_t p=1;p<513;) {
   uint32_t inode;uint16_t length;std::memcpy(&inode,&block[p],4);std::memcpy(&length,&block[p+4],2);
   assert(length>=12 && length%4==0 && p+length<=513);
   auto bytes=block[p+7];assert(bytes>0 && block[p+8+bytes]==0);
   std::string name(reinterpret_cast<char*>(&block[p+8]),bytes);assert(names.insert(name).second);
   if(name=="rooms")assert(block[p+6]==4 && inode==18);else assert(block[p+6]==8);
   p+=length;
  }
 }
 assert(names.size()==entries.size());
 directory.cursor=0;block.fill(0xa5);assert(GuestReadDirectory(directory,block.data()+1,511)<0 && directory.cursor==0 && block[1]==0xa5);
 assert(GuestReadDirectory(directory,nullptr,512)<0 && directory.cursor==0);
 auto previous=directory.bytes;entries.push_back({std::string(256,'x'),true,0});
 assert(!GuestPackDirectory(entries,&directory.bytes) && previous==directory.bytes);
 entries.clear();assert(GuestPackDirectory(entries,&directory.bytes) && directory.bytes.empty());
 assert(GuestReadDirectory(directory,block.data()+1,512)==0);
}