#pragma once
#include <cstdint>
#include <cstring>
namespace Libs::Graphics {
// Exact single-index point-list conversion; memory coherence is resolved by guest page faults.
inline bool DecodeSinglePointIndex(const void* source,uint32_t index_type,int32_t base_vertex,
 uint32_t* first_vertex) {
 if(!source || !first_vertex || index_type>2)return false;
 uint32_t index=0;
 if(index_type==0){uint16_t value;std::memcpy(&value,source,2);index=value;}
 else if(index_type==1)std::memcpy(&index,source,4);
 else {uint8_t value;std::memcpy(&value,source,1);index=value;}
 *first_vertex=index+static_cast<uint32_t>(base_vertex);return true;
}
}
