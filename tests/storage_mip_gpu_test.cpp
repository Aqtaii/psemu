#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vector>
#include <array>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
#define VKOK(x) CHECK((x)==VK_SUCCESS)
// PRODUCTION_INVERSE
int main(int argc,char** argv) {try {
 CHECK(argc==2);
 std::ifstream input(argv[1],std::ios::binary|std::ios::ate);CHECK(input);
 size_t shader_bytes=size_t(input.tellg());CHECK(shader_bytes && shader_bytes%4==0);
 std::vector<uint32_t> code(shader_bytes/4);input.seekg(0);input.read(reinterpret_cast<char*>(code.data()),shader_bytes);CHECK(input);
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
 VkInstance instance;VKOK(vkCreateInstance(&ici,nullptr,&instance));
 uint32_t physical_count=0;VKOK(vkEnumeratePhysicalDevices(instance,&physical_count,nullptr));CHECK(physical_count);
 std::vector<VkPhysicalDevice> physicals(physical_count);VKOK(vkEnumeratePhysicalDevices(instance,&physical_count,physicals.data()));
 VkPhysicalDevice physical=physicals[0];VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
 uint32_t family_count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,nullptr);
 std::vector<VkQueueFamilyProperties> families(family_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,families.data());
 uint32_t family=0;while(family<family_count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;CHECK(family<family_count);
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
 VkDevice device;VKOK(vkCreateDevice(physical,&dci,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceMemoryProperties memory_properties{};vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
 auto allocate=[&](VkMemoryRequirements requirements,VkMemoryPropertyFlags flags){uint32_t type=0;while(type<memory_properties.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || (memory_properties.memoryTypes[type].propertyFlags&flags)!=flags))++type;CHECK(type<memory_properties.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;VkDeviceMemory result;VKOK(vkAllocateMemory(device,&ai,nullptr,&result));return result;};
 constexpr uint32_t width=480,height=270,levels=8;constexpr VkDeviceSize bytes=0x160000;
 constexpr std::array<VkDeviceSize,8> offsets{0,0xc0000,0x100000,0x110000,0x120000,0x130000,0x140000,0x150000};
 constexpr std::array<uint32_t,8> pitches{512,256,128,128,128,128,128,128};
 VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=bytes;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 VkBuffer buffer;VKOK(vkCreateBuffer(device,&bci,nullptr,&buffer));VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer,&requirements);
 VkDeviceMemory buffer_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VKOK(vkBindBufferMemory(device,buffer,buffer_memory,0));
 void* mapped=nullptr;VKOK(vkMapMemory(device,buffer_memory,0,bytes,0,&mapped));auto* pixels=static_cast<uint32_t*>(mapped);
 std::fill(pixels,pixels+bytes/4,0xdeadbeef);std::array<VkBufferImageCopy,8> copies{};
 for(uint32_t l=0;l<levels;++l){auto& c=copies[l];c.bufferOffset=offsets[l];c.bufferRowLength=pitches[l];c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1};c.imageExtent={std::max(width>>l,1u),std::max(height>>l,1u),1};for(uint32_t y=0;y<c.imageExtent.height;++y)for(uint32_t x=0;x<c.imageExtent.width;++x){float value=0.125f+float(l)/16.f+float(x+y)/4096.f;std::memcpy(pixels+offsets[l]/4+y*pitches[l]+x,&value,4);}}
 std::vector<uint32_t> expected(pixels,pixels+bytes/4);
 VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imci.imageType=VK_IMAGE_TYPE_2D;imci.format=VK_FORMAT_R32_SFLOAT;imci.extent={width,height,1};imci.mipLevels=levels;imci.arrayLayers=1;imci.samples=VK_SAMPLE_COUNT_1_BIT;imci.tiling=VK_IMAGE_TILING_OPTIMAL;imci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 VkImage image;VKOK(vkCreateImage(device,&imci,nullptr,&image));vkGetImageMemoryRequirements(device,image,&requirements);VkDeviceMemory image_memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKOK(vkBindImageMemory(device,image,image_memory,0));
 VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vci.image=image;vci.viewType=VK_IMAGE_VIEW_TYPE_2D;vci.format=VK_FORMAT_R32_SFLOAT;vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageView view;VKOK(vkCreateImageView(device,&vci,nullptr,&view));
 VkImageViewUsageCreateInfo sampled_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};sampled_usage.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
 VkImageViewCreateInfo sci=vci;sci.pNext=&sampled_usage;sci.subresourceRange.levelCount=levels;sci.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
 VkImageView sampled_view;VKOK(vkCreateImageView(device,&sci,nullptr,&sampled_view));
 VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler_info.magFilter=VK_FILTER_NEAREST;sampler_info.minFilter=VK_FILTER_NEAREST;sampler_info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler_info.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.maxLod=7;
 VkSampler sampler;VKOK(vkCreateSampler(device,&sampler_info,nullptr,&sampler));
 VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
 VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};slci.bindingCount=2;slci.pBindings=bindings;VkDescriptorSetLayout set_layout;VKOK(vkCreateDescriptorSetLayout(device,&slci,nullptr,&set_layout));
 VkDescriptorPoolSize pool_sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=2;dpci.pPoolSizes=pool_sizes;VkDescriptorPool pool;VKOK(vkCreateDescriptorPool(device,&dpci,nullptr,&pool));
 VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&set_layout;VkDescriptorSet set;VKOK(vkAllocateDescriptorSets(device,&dsai,&set));
 VkDescriptorImageInfo descriptor{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&descriptor;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 VkDescriptorImageInfo sampled_descriptor{sampler,sampled_view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet sampled_write=write;sampled_write.dstBinding=1;sampled_write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;sampled_write.pImageInfo=&sampled_descriptor;vkUpdateDescriptorSets(device,1,&sampled_write,0,nullptr);
 VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT,0,4};VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&set_layout;plci.pushConstantRangeCount=1;plci.pPushConstantRanges=&push_range;VkPipelineLayout pipeline_layout;VKOK(vkCreatePipelineLayout(device,&plci,nullptr,&pipeline_layout));
 VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=shader_bytes;smci.pCode=code.data();VkShaderModule shader;VKOK(vkCreateShaderModule(device,&smci,nullptr,&shader));
 VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};cpci.layout=pipeline_layout;VkPipeline pipeline;VKOK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
 VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=family;VkCommandPool command_pool;VKOK(vkCreateCommandPool(device,&cpi,nullptr,&command_pool));VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbai.commandPool=command_pool;cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbai.commandBufferCount=1;VkCommandBuffer command;VKOK(vkAllocateCommandBuffers(device,&cbai,&command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(command,&begin));
 auto barrier=[&](VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags src_stage,VkPipelineStageFlags dst_stage){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=old_layout;b.newLayout=new_layout;b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1};vkCmdPipelineBarrier(command,src_stage,dst_stage,0,0,nullptr,0,nullptr,1,&b);};
 barrier(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 vkCmdCopyBufferToImage(command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,levels,copies.data());
 barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
 int source=int(InverseStorageSwizzleComponent(0x324,0));CHECK(source==0 && InverseStorageSwizzleComponent(0x324,1)==UINT32_MAX);
 vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&source);vkCmdDispatch(command,(width+7)/8,(height+7)/8,1);
 barrier(VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,levels,copies.data());
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);VKOK(vkEndCommandBuffer(command));
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;VKOK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
 std::vector<bool> visible(bytes/4,false);size_t changed=0;
 for(uint32_t l=0;l<levels;++l)for(uint32_t y=0;y<copies[l].imageExtent.height;++y)for(uint32_t x=0;x<copies[l].imageExtent.width;++x){size_t index=offsets[l]/4+y*pitches[l]+x;visible[index]=true;CHECK(pixels[index]==(l==0?0x3e800000u:expected[index]));changed+=l==0;}
 for(size_t i=0;i<visible.size();++i)if(!visible[i])CHECK(pixels[i]==expected[i]);
 std::printf("[STORAGE-MIP-GPU] device=%s R32Float RRR1 source=%d mip0_pixels=%zu unchanged_mips=7 sampled_mips=8 sampled_RRR1=checked padding_preserved=1 Astro=not-executed\n",properties.deviceName,source,changed);
 vkDestroyCommandPool(device,command_pool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);vkDestroySampler(device,sampler,nullptr);vkDestroyImageView(device,sampled_view,nullptr);vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,image_memory,nullptr);vkUnmapMemory(device,buffer_memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,buffer_memory,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
 return 0;
 }catch(const std::exception& e){std::printf("[STORAGE-MIP-GPU] FAIL: %s\n",e.what());return 1;}}

