#pragma once
#include <vulkan/vulkan.h>
namespace Libs::Graphics {
// Source is the coherent GPU owner returned by ObtainBuffer, never a CPU shadow.
template<class Dispatch>
inline void RecordGpuOwnedBufferBounce(VkCommandBuffer command,VkBuffer source,VkDeviceSize source_offset,
 VkBuffer destination,VkDeviceSize destination_offset,VkDeviceSize bytes,const Dispatch& dispatch) {
 VkBufferMemoryBarrier barriers[2]{};
 for(auto& b:barriers) {
  b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.size=bytes;
 }
 barriers[0].buffer=source;barriers[0].offset=source_offset;barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
 barriers[1].buffer=destination;barriers[1].offset=destination_offset;barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 dispatch.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,2,barriers,0,nullptr);
 const VkBufferCopy copy{source_offset,destination_offset,bytes};
 dispatch.vkCmdCopyBuffer(command,source,destination,1,&copy);
 auto after=barriers[1];after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 after.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
 dispatch.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,1,&after,0,nullptr);
}
} // namespace Libs::Graphics
