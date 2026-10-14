#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
// KytyPS5 FreeBSD dirent layout: inode32, reclen16, type8, namlen8, name;
// entries align to4 bytes and the last entry in each512-byte block covers padding.
struct GuestDirectoryEntry {std::string name;bool file=true;uint32_t inode=0;};
struct GuestDirectorySnapshot {std::string path;std::vector<uint8_t> bytes;size_t cursor=0;};
inline bool GuestPackDirectory(const std::vector<GuestDirectoryEntry>& entries,std::vector<uint8_t>* output) {
 if(!output)return false;
 std::vector<uint8_t> packed;size_t offset=0,last=0;
 for(const auto& entry:entries) {
  if(entry.name.empty() || entry.name.size()>255 || entry.name.find('\0')!=std::string::npos)return false;
  const size_t record=(8+entry.name.size()+1+3)&~size_t(3);
  if(offset+record>packed.size()) {
   if(!packed.empty()) {
    uint16_t previous;std::memcpy(&previous,packed.data()+last,2);
    previous=uint16_t(previous+packed.size()-offset);std::memcpy(packed.data()+last,&previous,2);
   }
   offset=packed.size();packed.resize(offset+512,0);
  }
  const auto length=uint16_t(record);
  std::memcpy(packed.data()+offset,&entry.inode,4);std::memcpy(packed.data()+offset+4,&length,2);
  packed[offset+6]=entry.file ? 8 : 4;packed[offset+7]=uint8_t(entry.name.size());
  std::memcpy(packed.data()+offset+8,entry.name.c_str(),entry.name.size()+1);
  last=offset+4;offset+=record;
 }
 if(!packed.empty()) {
  uint16_t previous;std::memcpy(&previous,packed.data()+last,2);
  previous=uint16_t(previous+packed.size()-offset);std::memcpy(packed.data()+last,&previous,2);
 }
 *output=std::move(packed);return true;
}
inline int64_t GuestReadDirectory(GuestDirectorySnapshot& directory,void* buffer,int bytes,int64_t* base=nullptr) {
 if(!buffer)return -2147352562LL; // EFAULT,0x8002000e
 if(bytes<512)return -2147352554LL; // EINVAL
 if(base)*base=int64_t(directory.cursor);
 const auto remaining=directory.cursor<directory.bytes.size() ? directory.bytes.size()-directory.cursor : 0;
 const auto count=(std::min)(remaining,size_t(bytes)&~size_t(511));
 if(count)std::memcpy(buffer,directory.bytes.data()+directory.cursor,count);
 directory.cursor+=count;return int64_t(count);
}