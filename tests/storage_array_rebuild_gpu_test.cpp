#define NOMINMAX
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
#define VKOK(x) CHECK((x)==VK_SUCCESS)
int main(int argc,char** argv){try{
 CHECK(argc==2);std::ifstream file(argv[1],std::ios::binary|std::ios::ate);CHECK(file);
 const auto shader_bytes=size_t(file.tellg());CHECK(shader_bytes && shader_bytes%4==0);
 std::vector<uint32_t> code(shader_bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),shader_bytes);CHECK(file);
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
 VkInstance instance;VKOK(vkCreateInstance(&ici,nullptr,&instance));
 uint32_t count=0;VKOK(vkEnumeratePhysicalDevices(instance,&count,nullptr));CHECK(count);
 std::vector<VkPhysicalDevice> devices(count);VKOK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));const auto physical=devices[0];
 VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
 vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
 uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;CHECK(family<count);
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
 VkDevice device;VKOK(vkCreateDevice(physical,&dci,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceMemoryProperties memory_properties{};vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
 auto allocate=[&](VkMemoryRequirements requirements,VkMemoryPropertyFlags flags){uint32_t type=0;while(type<memory_properties.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || (memory_properties.memoryTypes[type].propertyFlags&flags)!=flags))++type;CHECK(type<memory_properties.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;VkDeviceMemory memory;VKOK(vkAllocateMemory(device,&ai,nullptr,&memory));return memory;};
 constexpr uint32_t width=10240,height=320;constexpr VkDeviceSize slice=VkDeviceSize(width)*height*8,payload=slice*2,bytes=payload+256;
 CHECK(payload<=properties.limits.maxStorageBufferRange);
 VkFormatProperties format_properties{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R16G16B16A16_SFLOAT,&format_properties);
 CHECK(format_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);CHECK(format_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
 struct Buffer{VkBuffer handle;VkDeviceMemory memory;uint32_t* words;};
 auto make_buffer=[&](VkBufferUsageFlags usage){Buffer b{};VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;VKOK(vkCreateBuffer(device,&ci,nullptr,&b.handle));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,b.handle,&requirements);b.memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VKOK(vkBindBufferMemory(device,b.handle,b.memory,0));void* mapped;VKOK(vkMapMemory(device,b.memory,0,bytes,0,&mapped));b.words=static_cast<uint32_t*>(mapped);return b;};
 auto input=make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);auto output=make_buffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 std::fill(input.words,input.words+bytes/4,0xdeadbeefu);std::fill(output.words,output.words+bytes/4,0xdeadbeefu);
 std::fill(input.words,input.words+slice/4,0x30003000u);std::fill(input.words+slice/4,input.words+payload/4,0x3c003a00u);
 struct Image{VkImage handle;VkDeviceMemory memory;};
 auto make_image=[&](uint32_t layers){Image image{};VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R16G16B16A16_SFLOAT;ci.extent={width,height,1};ci.mipLevels=1;ci.arrayLayers=layers;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;VKOK(vkCreateImage(device,&ci,nullptr,&image.handle));VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,image.handle,&requirements);image.memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKOK(vkBindImageMemory(device,image.handle,image.memory,0));return image;};
 auto old_image=make_image(1);auto expanded=make_image(2);
 VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;VkCommandPool pool;VKOK(vkCreateCommandPool(device,&pci,nullptr,&pool));
 VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer command;VKOK(vkAllocateCommandBuffers(device,&cai,&command));
 auto begin=[&]{VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(command,&info));};
 auto submit=[&]{VKOK(vkEndCommandBuffer(command));VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;VKOK(vkQueueSubmit(queue,1,&info,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));};
 auto image_barrier=[&](VkImage image,uint32_t layers,VkImageLayout before,VkImageLayout after,VkAccessFlags source,VkAccessFlags target,VkPipelineStageFlags source_stage,VkPipelineStageFlags target_stage){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=before;b.newLayout=after;b.srcAccessMask=source;b.dstAccessMask=target;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};vkCmdPipelineBarrier(command,source_stage,target_stage,0,0,nullptr,0,nullptr,1,&b);};
 begin();image_barrier(old_image.handle,1,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkClearColorValue clear{};clear.float32[0]=0.25f;clear.float32[1]=0.5f;clear.float32[2]=clear.float32[3]=1.f;const VkImageSubresourceRange one_layer{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,old_image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&one_layer);
 image_barrier(old_image.handle,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkBufferImageCopy prefix{};prefix.bufferRowLength=width;prefix.bufferImageHeight=height;prefix.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};prefix.imageExtent={width,height,1};vkCmdCopyImageToBuffer(command,old_image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,input.handle,1,&prefix);
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);submit();
 for(size_t i=0;i<slice/4;++i)CHECK(input.words[i]==(i%2?0x3c003c00u:0x38003400u));
 for(size_t i=slice/4;i<payload/4;++i)CHECK(input.words[i]==0x3c003a00u);
 for(size_t i=payload/4;i<bytes/4;++i)CHECK(input.words[i]==0xdeadbeefu);
 VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vci.image=expanded.handle;vci.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;vci.format=VK_FORMAT_R16G16B16A16_SFLOAT;vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};VkImageView view;VKOK(vkCreateImageView(device,&vci,nullptr,&view));
 VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sci.magFilter=sci.minFilter=VK_FILTER_NEAREST;sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;VkSampler sampler;VKOK(vkCreateSampler(device,&sci,nullptr,&sampler));
 VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};slci.bindingCount=2;slci.pBindings=bindings;VkDescriptorSetLayout set_layout;VKOK(vkCreateDescriptorSetLayout(device,&slci,nullptr,&set_layout));
 VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=2;dpci.pPoolSizes=sizes;VkDescriptorPool descriptor_pool;VKOK(vkCreateDescriptorPool(device,&dpci,nullptr,&descriptor_pool));VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=descriptor_pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&set_layout;VkDescriptorSet set;VKOK(vkAllocateDescriptorSets(device,&dsai,&set));
 VkDescriptorImageInfo image_info{sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo buffer_info{output.handle,0,payload};VkWriteDescriptorSet writes[2]{};writes[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&image_info,nullptr,nullptr};writes[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&buffer_info,nullptr};vkUpdateDescriptorSets(device,2,writes,0,nullptr);
 VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&set_layout;VkPipelineLayout pipeline_layout;VKOK(vkCreatePipelineLayout(device,&plci,nullptr,&pipeline_layout));VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=shader_bytes;smci.pCode=code.data();VkShaderModule shader;VKOK(vkCreateShaderModule(device,&smci,nullptr,&shader));VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};cpci.layout=pipeline_layout;VkPipeline pipeline;VKOK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
 VKOK(vkResetCommandPool(device,pool,0));begin();image_barrier(expanded.handle,2,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};upload.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;upload.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&upload,0,nullptr,0,nullptr);
 std::array<VkBufferImageCopy,2> copies{prefix,prefix};copies[1].bufferOffset=slice;copies[1].imageSubresource.baseArrayLayer=1;vkCmdCopyBufferToImage(command,input.handle,expanded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,copies.data());
 image_barrier(expanded.handle,2,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
 vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdDispatch(command,(width+7)/8,(height+7)/8,2);
 host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);submit();
 for(size_t i=0;i<payload/4;++i)CHECK(output.words[i]==(i<slice/4?(i%2?0x3c003c00u:0x38003400u):0x3c003a00u));
 for(size_t i=payload/4;i<bytes/4;++i)CHECK(output.words[i]==0xdeadbeefu && input.words[i]==0xdeadbeefu);
 std::printf("[STORAGE-ARRAY-REBUILD-GPU] device=%s RGBA16F10240x320 layers1to2 gpu_prefix=%llu cpu_tail=%llu sampled_all_texels=1 guards_preserved=1 fixture_shader=GLSL Astro=not-executed\n",properties.deviceName,(unsigned long long)slice,(unsigned long long)slice);
 vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorPool(device,descriptor_pool,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);vkDestroySampler(device,sampler,nullptr);vkDestroyImageView(device,view,nullptr);vkDestroyCommandPool(device,pool,nullptr);
 for(auto image:{old_image,expanded}){vkDestroyImage(device,image.handle,nullptr);vkFreeMemory(device,image.memory,nullptr);}for(auto b:{input,output}){vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.handle,nullptr);vkFreeMemory(device,b.memory,nullptr);}vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"[STORAGE-ARRAY-REBUILD-GPU] FAIL %s\n",e.what());return 1;}}
