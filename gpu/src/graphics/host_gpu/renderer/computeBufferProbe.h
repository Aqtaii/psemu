#pragma once
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

namespace Libs::Graphics {
inline int AstroProducerProbeKind(uint64_t hash, uint32_t words) {
    if (hash == 0x25ea92806a8f33d1ull && words == 659) return 0;
    if (hash == 0x1cbf799b3cec5a07ull && words == 5798) return 1;
    return -1;
}
inline bool AstroProducerProbeOrdinal(uint32_t ordinal) {
    return ordinal < 8 || (ordinal <= 8192 &&
        ((ordinal & (ordinal - 1)) == 0 || ordinal % 64 == 0));
}
// Check both the descriptor window and its owning allocation, without overflowing.
inline bool AstroProducerProbeOffset(uint64_t view_offset, uint64_t view_bytes,
    uint64_t allocation_bytes, uint32_t stride, uint32_t records, uint32_t index,
    uint64_t* result) {
    if (!result || stride != 128 || records != 12288 || index >= records ||
        view_offset % 4 || view_offset > allocation_bytes ||
        view_bytes > allocation_bytes - view_offset) return false;
    const uint64_t relative = uint64_t(index) * stride;
    if (relative > view_bytes || 128 > view_bytes - relative) return false;
    *result = view_offset + relative;
    return true;
}
// Full parent/constant records requested for this exact transform producer.
inline bool AstroProducerInputProbeOffset(uint64_t view_offset,uint64_t view_bytes,
 uint64_t allocation_bytes,uint32_t stride,uint32_t records,uint32_t slot,uint32_t index,
 uint64_t* result) {
 const bool parent=slot==4 && stride==432 && index==0x7fe;
 const bool constants=slot==3 && stride==276 && index==0x3a00;
 if(!result || (!parent && !constants) || index>=records || view_offset%4 ||
    view_offset>allocation_bytes || view_bytes>allocation_bytes-view_offset)return false;
 const uint64_t relative=uint64_t(index)*stride;
 if(relative>view_bytes || stride>view_bytes-relative)return false;
 *result=view_offset+relative;return true;
}
struct AstroProducerBufferCopy {
    VkBuffer source;
    VkDeviceSize source_offset, destination_offset;
    VkDeviceSize bytes=128;
};
// Shared with the Vulkan regression: exact commands used by the opt-in capture.
template<class Dispatch>
inline void RecordAstroProducerBufferCopies(VkCommandBuffer command, VkBuffer staging,
    VkDeviceSize staging_bytes, std::span<const AstroProducerBufferCopy> copies, const Dispatch& dispatch) {
    VkMemoryBarrier readable{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    readable.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
    readable.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    dispatch.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,1,&readable,0,nullptr,0,nullptr);
    for (const auto& copy : copies) {
        const VkBufferCopy region{copy.source_offset,copy.destination_offset,copy.bytes};
        dispatch.vkCmdCopyBuffer(command,copy.source,staging,1,&region);
    }
    VkMemoryBarrier resume{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    resume.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    resume.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    dispatch.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,1,&resume,0,nullptr,0,nullptr);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=staging;host.offset=0;host.size=staging_bytes;
    dispatch.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,nullptr,1,&host,0,nullptr);
}
} // namespace Libs::Graphics
