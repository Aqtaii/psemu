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
#include <cstdlib>
#include <atomic>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
#define VKOK(x) CHECK((x)==VK_SUCCESS)
static std::atomic<uint32_t> validation_issues{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT,
 VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
 validation_issues.fetch_add(1,std::memory_order_relaxed);
 std::fprintf(stderr,"[STORAGE-VALIDATION] %s\n",data && data->pMessage ? data->pMessage : "missing diagnostic");return VK_FALSE;
}
// PRODUCTION_INVERSE
// PRODUCTION_STANDARD4KB
// PRODUCTION_R32_ALIAS
int main(int argc,char** argv) {try {
 CHECK(argc>=2 && argc<=4);const bool depth_rebuild=argc==4 && std::strcmp(argv[3],"depth-rebuild")==0;const bool standard4kb=argc==4 && std::strcmp(argv[3],"standard4kb")==0;const bool atomic_emitter=argc==4 && std::strcmp(argv[3],"atomic-emitter")==0;const bool atomic_image=atomic_emitter || (argc==4 && std::strcmp(argv[3],"atomic")==0);const bool packed=argc==4 && std::strcmp(argv[3],"packed")==0;const bool unorm=argc==4 && std::strcmp(argv[3],"unorm-linear")==0;const bool full_tail=argc==4 && std::strcmp(argv[3],"rgba16-tail")==0;const bool rgba16=standard4kb || unorm || full_tail || (argc==4 && std::strcmp(argv[3],"rgba16")==0);const uint32_t target=argc>=3?uint32_t(std::strtoul(argv[2],nullptr,10)):0;CHECK(target<8);
 std::ifstream input(argv[1],std::ios::binary|std::ios::ate);CHECK(input);
 size_t shader_bytes=size_t(input.tellg());CHECK(shader_bytes && shader_bytes%4==0);
 std::vector<uint32_t> code(shader_bytes/4);input.seekg(0);input.read(reinterpret_cast<char*>(code.data()),shader_bytes);CHECK(input);
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
 const char* validation_layer="VK_LAYER_KHRONOS_validation";const char* requested_layers=std::getenv("VK_INSTANCE_LAYERS");
 if(requested_layers && std::strstr(requested_layers,validation_layer)){ici.enabledLayerCount=1;ici.ppEnabledLayerNames=&validation_layer;}
 uint32_t extension_count=0;VKOK(vkEnumerateInstanceExtensionProperties(nullptr,&extension_count,nullptr));
 std::vector<VkExtensionProperties> extensions(extension_count);VKOK(vkEnumerateInstanceExtensionProperties(nullptr,&extension_count,extensions.data()));
 bool debug_utils=false;for(const auto& extension:extensions)if(std::strcmp(extension.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)==0)debug_utils=true;
 const char* debug_extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
 VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
 debug_info.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
 debug_info.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
 debug_info.pfnUserCallback=ValidationMessage;
 if(debug_utils){ici.enabledExtensionCount=1;ici.ppEnabledExtensionNames=&debug_extension;ici.pNext=&debug_info;}
 VkInstance instance;VKOK(vkCreateInstance(&ici,nullptr,&instance));VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
 if(debug_utils){const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
  if(!create)throw std::runtime_error("Debug utils entry unavailable");VKOK(create(instance,&debug_info,nullptr,&messenger));}

 uint32_t physical_count=0;VKOK(vkEnumeratePhysicalDevices(instance,&physical_count,nullptr));CHECK(physical_count);
 std::vector<VkPhysicalDevice> physicals(physical_count);VKOK(vkEnumeratePhysicalDevices(instance,&physical_count,physicals.data()));
 VkPhysicalDevice physical=physicals[0];VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
 uint32_t family_count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,nullptr);
 std::vector<VkQueueFamilyProperties> families(family_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,families.data());
 uint32_t family=0;while(family<family_count && ((families[family].queueFlags & (VK_QUEUE_COMPUTE_BIT | (depth_rebuild?VK_QUEUE_GRAPHICS_BIT:0))) != (VK_QUEUE_COMPUTE_BIT | (depth_rebuild?VK_QUEUE_GRAPHICS_BIT:0))))++family;CHECK(family<family_count);
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
 VkPhysicalDeviceFeatures enabled{};
 if(atomic_emitter){
  VkPhysicalDeviceFeatures supported{};vkGetPhysicalDeviceFeatures(physical,&supported);
  auto capability=[&](uint32_t value){for(size_t i=5;i<code.size();){const auto count=code[i]>>16;CHECK(count && count<=code.size()-i);if((code[i]&65535)==17 && count==2 && code[i+1]==value)return true;i+=count;}return false;};
  if(capability(11)){CHECK(supported.shaderInt64);enabled.shaderInt64=VK_TRUE;}
  if(capability(10)){CHECK(supported.shaderFloat64);enabled.shaderFloat64=VK_TRUE;}
  if(capability(55)){CHECK(supported.shaderStorageImageReadWithoutFormat);enabled.shaderStorageImageReadWithoutFormat=VK_TRUE;}
  if(capability(56)){CHECK(supported.shaderStorageImageWriteWithoutFormat);enabled.shaderStorageImageWriteWithoutFormat=VK_TRUE;}
  dci.pEnabledFeatures=&enabled;
 }

 if(packed || rgba16){VkPhysicalDeviceFeatures supported{};vkGetPhysicalDeviceFeatures(physical,&supported);CHECK(supported.shaderStorageImageWriteWithoutFormat);enabled.shaderStorageImageWriteWithoutFormat=VK_TRUE;dci.pEnabledFeatures=&enabled;}
 VkDevice device;VKOK(vkCreateDevice(physical,&dci,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceMemoryProperties memory_properties{};vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
 auto allocate=[&](VkMemoryRequirements requirements,VkMemoryPropertyFlags flags){uint32_t type=0;while(type<memory_properties.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || (memory_properties.memoryTypes[type].propertyFlags&flags)!=flags))++type;CHECK(type<memory_properties.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;VkDeviceMemory result;VKOK(vkAllocateMemory(device,&ai,nullptr,&result));return result;};
 const uint32_t width=depth_rebuild?960:standard4kb?32:(unorm?960:((packed||rgba16)?1920:480)),height=depth_rebuild?540:standard4kb?32:(unorm?540:((packed||rgba16)?1080:270)),levels=(depth_rebuild||standard4kb||packed||unorm||atomic_image)?1:(full_tail?11:(rgba16?7:8)),sampled_levels=full_tail?6:levels,words_per_pixel=rgba16?2:1;VkDeviceSize bytes=packed?0x870000:0x160000;CHECK(target<levels);
 const VkFormat format=atomic_image?VK_FORMAT_R32_UINT:packed?VK_FORMAT_B10G11R11_UFLOAT_PACK32:(unorm?VK_FORMAT_R16G16B16A16_UNORM:(rgba16?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R32_SFLOAT));
 VkFormatProperties format_properties{};vkGetPhysicalDeviceFormatProperties(physical,format,&format_properties);CHECK(format_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);if(atomic_image)CHECK(format_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT);
 std::array<VkDeviceSize,16> offsets{0,0xc0000,0x100000,0x110000,0x120000,0x130000,0x140000,0x150000};
 std::array<uint32_t,16> pitches{512,256,128,128,128,128,128,128};
 if(packed)pitches[0]=1920;
 if(depth_rebuild){bytes=0x280000;pitches[0]=1024;}
 if(rgba16){bytes=0;for(uint32_t l=0;l<levels;l++){offsets[l]=bytes;pitches[l]=std::max(width>>l,1u);bytes=(bytes+VkDeviceSize(pitches[l])*std::max(height>>l,1u)*8+255)&~VkDeviceSize(255);}}
 VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=bytes;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 VkBuffer buffer;VKOK(vkCreateBuffer(device,&bci,nullptr,&buffer));VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer,&requirements);
 VkDeviceMemory buffer_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VKOK(vkBindBufferMemory(device,buffer,buffer_memory,0));
 void* mapped=nullptr;VKOK(vkMapMemory(device,buffer_memory,0,bytes,0,&mapped));auto* pixels=static_cast<uint32_t*>(mapped);
 std::fill(pixels,pixels+bytes/4,0xdeadbeef);std::array<VkBufferImageCopy,16> copies{};
 for(uint32_t l=0;l<levels;++l){auto& c=copies[l];c.bufferOffset=offsets[l];c.bufferRowLength=pitches[l];c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1};c.imageExtent={std::max(width>>l,1u),std::max(height>>l,1u),1};for(uint32_t y=0;y<c.imageExtent.height;++y)for(uint32_t x=0;x<c.imageExtent.width;++x){float value=0.125f+float(l)/16.f+float(x+y)/4096.f;const auto index=offsets[l]/4+(y*pitches[l]+x)*words_per_pixel;if(atomic_image)pixels[index]=0;else if(rgba16){pixels[index]=pixels[index+1]=unorm?0u:0x30003000u;}else std::memcpy(pixels+index,&value,4);}}
 std::vector<uint32_t> expected(pixels,pixels+bytes/4);
 VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imci.imageType=VK_IMAGE_TYPE_2D;imci.format=format;imci.extent={width,height,1};imci.mipLevels=levels;imci.arrayLayers=1;imci.samples=VK_SAMPLE_COUNT_1_BIT;imci.tiling=VK_IMAGE_TILING_OPTIMAL;imci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 VkImage image;VKOK(vkCreateImage(device,&imci,nullptr,&image));vkGetImageMemoryRequirements(device,image,&requirements);VkDeviceMemory image_memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKOK(vkBindImageMemory(device,image,image_memory,0));
 VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vci.image=image;vci.viewType=VK_IMAGE_VIEW_TYPE_2D;vci.format=format;vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,target,1,0,1};
 VkImageView view;VKOK(vkCreateImageView(device,&vci,nullptr,&view));
 VkImageViewUsageCreateInfo sampled_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};sampled_usage.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
 VkImageViewCreateInfo sci=vci;sci.pNext=&sampled_usage;sci.subresourceRange.baseMipLevel=0;sci.subresourceRange.levelCount=sampled_levels;sci.components=rgba16?VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY}:packed?VkComponentMapping{VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_ONE}:VkComponentMapping{VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
 VkImageView sampled_view;VKOK(vkCreateImageView(device,&sci,nullptr,&sampled_view));
 VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler_info.magFilter=VK_FILTER_NEAREST;sampler_info.minFilter=VK_FILTER_NEAREST;sampler_info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler_info.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler_info.maxLod=7;
 VkSampler sampler;VKOK(vkCreateSampler(device,&sampler_info,nullptr,&sampler));
 VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
 VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};slci.bindingCount=2;slci.pBindings=bindings;VkDescriptorSetLayout set_layout;VKOK(vkCreateDescriptorSetLayout(device,&slci,nullptr,&set_layout));
 VkDescriptorPoolSize pool_sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=2;dpci.pPoolSizes=pool_sizes;VkDescriptorPool pool;VKOK(vkCreateDescriptorPool(device,&dpci,nullptr,&pool));
 VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&set_layout;VkDescriptorSet set;VKOK(vkAllocateDescriptorSets(device,&dsai,&set));
 VkDescriptorImageInfo descriptor{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&descriptor;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 VkDescriptorImageInfo sampled_descriptor{sampler,sampled_view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet sampled_write=write;sampled_write.dstBinding=1;sampled_write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;sampled_write.pImageInfo=&sampled_descriptor;vkUpdateDescriptorSets(device,1,&sampled_write,0,nullptr);
 VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT,0,8};VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&set_layout;plci.pushConstantRangeCount=1;plci.pPushConstantRanges=&push_range;VkPipelineLayout pipeline_layout;VKOK(vkCreatePipelineLayout(device,&plci,nullptr,&pipeline_layout));
 VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=shader_bytes;smci.pCode=code.data();VkShaderModule shader;VKOK(vkCreateShaderModule(device,&smci,nullptr,&shader));
 VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};cpci.layout=pipeline_layout;VkPipeline pipeline;VKOK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
 VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=family;VkCommandPool command_pool;VKOK(vkCreateCommandPool(device,&cpi,nullptr,&command_pool));VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbai.commandPool=command_pool;cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbai.commandBufferCount=1;VkCommandBuffer command;VKOK(vkAllocateCommandBuffers(device,&cbai,&command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(command,&begin));
 auto barrier=[&](VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags src_stage,VkPipelineStageFlags dst_stage){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=old_layout;b.newLayout=new_layout;b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1};vkCmdPipelineBarrier(command,src_stage,dst_stage,0,0,nullptr,0,nullptr,1,&b);};
 barrier(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 if(!packed)vkCmdCopyBufferToImage(command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,levels,copies.data());
 if(packed){VkClearColorValue clear{};clear.float32[0]=clear.float32[1]=clear.float32[2]=1.f;clear.float32[3]=0.f;
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);}

 barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
 int source=int(InverseStorageSwizzleComponent((rgba16||atomic_image)?0xfac:(packed?0x3ac:0x324),0));CHECK(source==0);
 if(packed){CHECK(InverseStorageSwizzleComponent(0x3ac,1)==1 && InverseStorageSwizzleComponent(0x3ac,2)==2 && InverseStorageSwizzleComponent(0x3ac,3)==UINT32_MAX);}else if(rgba16||atomic_image){for(uint32_t c=0;c<4;c++)CHECK(InverseStorageSwizzleComponent(0xfac,c)==c);}else CHECK(InverseStorageSwizzleComponent(0x324,1)==UINT32_MAX);
 vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);const int push[]={depth_rebuild?-2:rgba16?(unorm?-int(sampled_levels):int(sampled_levels)):source,int(target)};vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,push);vkCmdDispatch(command,(std::max(width>>target,1u)+7)/8,(std::max(height>>target,1u)+7)/8,1);
 if(packed){
  barrier(VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  const int sample_push[]={source,1};vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,sample_push);vkCmdDispatch(command,(width+7)/8,(height+7)/8,1);
 }
 barrier(VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,levels,copies.data());
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);VKOK(vkEndCommandBuffer(command));
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;VKOK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
 std::vector<bool> visible(bytes/4,false);size_t changed=0;std::vector<bool> atomic_returns(atomic_image&&!atomic_emitter?size_t(width)*height:0,false);
 for(uint32_t l=0;l<levels;++l)for(uint32_t y=0;y<copies[l].imageExtent.height;++y)for(uint32_t x=0;x<copies[l].imageExtent.width;++x){size_t index=offsets[l]/4+(y*pitches[l]+x)*words_per_pixel;for(uint32_t c=0;c<words_per_pixel;c++){visible[index+c]=true;if(atomic_image){const auto value=pixels[index+c];if(index==0){const uint32_t count=atomic_emitter?((width+7)/8)*((height+7)/8)*64:width*height;if(value!=count)std::printf("[ATOMIC-COUNT] actual=%u expected=%u\n",value,count);CHECK(value==count);}else if(atomic_emitter)CHECK(value==0);else{CHECK(value<width*height);CHECK(!atomic_returns[value]);atomic_returns[value]=true;}}else{const uint32_t written=rgba16?(unorm?0xffff0000u:(c==0?0x38003400u:0x3c003c00u)):(packed?((15u<<5u)<<22u)|((14u<<6u)<<11u)|(13u<<6u):0x3e800000u);if(depth_rebuild){const float depth=0.25f+float((x+3*y)&1023u)/2048.f;uint32_t bits;std::memcpy(&bits,&depth,4);CHECK(pixels[index+c]==bits);}else CHECK(pixels[index+c]==(l==target?written:expected[index+c]));}}changed+=l==target;}
 for(size_t i=0;i<visible.size();++i)if(!visible[i])CHECK(pixels[i]==expected[i]);
 if(depth_rebuild) {
  const std::vector<uint32_t> gpu_values(pixels,pixels+bytes/4);
  VkFormatProperties depth_properties{};
  vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_D32_SFLOAT,&depth_properties);
  CHECK(depth_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
  for(const uint32_t source_tile : {24u,27u}) {
   std::vector<uint32_t> guest(bytes/4+2,0x7eab1234u), mask(bytes/4), ones(bytes/4,1), decoded(bytes/4);
   if(source_tile==24){
    TileConvertLinearToTiledDepthPreservePadding(guest.data()+1,gpu_values.data(),22,width,height,pitches[0],bytes);
    TileConvertLinearToTiledDepthPreservePadding(mask.data(),ones.data(),22,width,height,pitches[0],bytes);
   } else {
    TileConvertLinearToTiledRenderTarget32PreservePadding(guest.data()+1,gpu_values.data(),width,height,pitches[0],bytes);
    TileConvertLinearToTiledRenderTarget32PreservePadding(mask.data(),ones.data(),width,height,pitches[0],bytes);
   }
   CHECK(guest.front()==0x7eab1234u && guest.back()==0x7eab1234u);
   for(size_t i=0;i<mask.size();++i)if(!mask[i])CHECK(guest[i+1]==0x7eab1234u);
   TileConvertTiledToLinearDepth(decoded.data(),guest.data()+1,22,width,height,pitches[0],bytes);
   for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
    CHECK(decoded[y*pitches[0]+x]==gpu_values[y*pitches[0]+x]);
   std::memcpy(pixels,decoded.data(),size_t(bytes));
   VkImageCreateInfo depth_ci=imci;depth_ci.format=VK_FORMAT_D32_SFLOAT;
   depth_ci.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage depth_image;VKOK(vkCreateImage(device,&depth_ci,nullptr,&depth_image));
   vkGetImageMemoryRequirements(device,depth_image,&requirements);
   VkDeviceMemory depth_memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VKOK(vkBindImageMemory(device,depth_image,depth_memory,0));
   VkImageViewCreateInfo depth_view_ci=vci;depth_view_ci.image=depth_image;depth_view_ci.format=VK_FORMAT_D32_SFLOAT;
   depth_view_ci.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
   VkImageView depth_view;VKOK(vkCreateImageView(device,&depth_view_ci,nullptr,&depth_view));
   VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_D32_SFLOAT;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
   attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   VkAttachmentReference depth_ref{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
   VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.pDepthStencilAttachment=&depth_ref;
   VkRenderPassCreateInfo rp_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp_ci.attachmentCount=1;rp_ci.pAttachments=&attachment;rp_ci.subpassCount=1;rp_ci.pSubpasses=&subpass;
   VkRenderPass render_pass;VKOK(vkCreateRenderPass(device,&rp_ci,nullptr,&render_pass));
   VkFramebufferCreateInfo fb_ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb_ci.renderPass=render_pass;fb_ci.attachmentCount=1;fb_ci.pAttachments=&depth_view;fb_ci.width=width;fb_ci.height=height;fb_ci.layers=1;
   VkFramebuffer framebuffer;VKOK(vkCreateFramebuffer(device,&fb_ci,nullptr,&framebuffer));
   VKOK(vkResetCommandPool(device,command_pool,0));VKOK(vkBeginCommandBuffer(command,&begin));
   auto depth_barrier=[&](VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags src_stage,VkPipelineStageFlags dst_stage){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=before;b.newLayout=after;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=depth_image;b.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    vkCmdPipelineBarrier(command,src_stage,dst_stage,0,0,nullptr,0,nullptr,1,&b);
   };
   VkBufferImageCopy depth_copy=copies[0];depth_copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
   depth_barrier(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyBufferToImage(command,buffer,depth_image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&depth_copy);
   constexpr auto depth_stages=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
   depth_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,depth_stages);
   VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp_begin.renderPass=render_pass;rp_begin.framebuffer=framebuffer;rp_begin.renderArea.extent={width,height};
   vkCmdBeginRenderPass(command,&rp_begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(command);
   depth_barrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,depth_stages,VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyImageToBuffer(command,depth_image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&depth_copy);
   vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
   VKOK(vkEndCommandBuffer(command));VKOK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
   for(size_t i=0;i<decoded.size();++i)CHECK(pixels[i]==decoded[i]);
   std::printf("[STORAGE-DEPTH-ATTACHMENT-GPU] device=%s source_tile=%u extent=960x540 gpu_storage_write=spatial_pattern production_retile_detile=bit_exact D32_readonly_load_store=bit_exact backing_padding_guards=preserved fixture=synthetic Astro=not-executed\n",properties.deviceName,source_tile);
   vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,render_pass,nullptr);vkDestroyImageView(device,depth_view,nullptr);
   vkDestroyImage(device,depth_image,nullptr);vkFreeMemory(device,depth_memory,nullptr);
  }
 }
 if(standard4kb) {
  std::vector<uint64_t> tiled(1032,0xdeadbeefcafebabeull),linear(1032,0xdeadbeefcafebabeull);
  TileConvertLinearToTiledStandard4KB64(tiled.data(),pixels+offsets[0]/4,32,32,32,8192);
  DetileStandard4KBTyped<uint64_t>(linear.data(),tiled.data(),32,32,32,32,5,4,8192,8192,0,0);
  for(uint32_t i=0;i<1024;++i)CHECK(linear[i]==0x3c003c0038003400ull);
  for(uint32_t i=1024;i<1032;++i)CHECK(tiled[i]==0xdeadbeefcafebabeull && linear[i]==0xdeadbeefcafebabeull);
  std::printf("[STANDARD4KB-STORAGE-GPU] device=%s RGBA16F32x32 stored_pixels=1024 production_retile_detile=bit_exact guards_preserved=1 fixture_shader=GLSL Astro=not-executed\n",properties.deviceName);
 }
 if(atomic_emitter)std::printf("[STORAGE-ATOMIC-PRODUCTION-GPU] device=%s R32Uint2D R001 counter=%u other_texels_unchanged=1 padding_preserved=1 production_emitter=1 Astro=not-executed\n",properties.deviceName,((width+7)/8)*((height+7)/8)*64);
 else if(atomic_image){CHECK(std::count(atomic_returns.begin(),atomic_returns.end(),true)==size_t(width)*height-1);std::printf("[STORAGE-ATOMIC-GPU] device=%s R32Uint2D counter=%u unique_old_values=%u padding_preserved=1 fixture_shader=GLSL Astro=not-executed\n",properties.deviceName,width*height,width*height-1);}
 else if(packed)std::printf("[STORAGE-PACKED-GPU] device=%s R11G11B10 RGB1 identity_storage formatless_write=1 native_clear=checked sampled_RGB1=checked written_pixels=%zu packed_bits=checked padding_preserved=1 Astro=not-executed\n",properties.deviceName,changed);
 else if(rgba16)std::printf("[STORAGE-RGBA16-MIP-GPU] device=%s format=%s target_mip=%u written_pixels=%zu backing_levels=%u unchanged_mips=%u sampled_levels=%u identity_RGBA=checked component_bits=checked padding_preserved=1 Astro=not-executed\n",properties.deviceName,unorm?"UNorm-linear":"Sfloat",target,changed,levels,levels-1,sampled_levels);
 else if(!depth_rebuild)std::printf("[STORAGE-MIP-GPU] device=%s R32Float RRR1 source=%d target_mip=%u written_pixels=%zu unchanged_mips=7 sampled_mips=8 sampled_RRR1=checked padding_preserved=1 Astro=not-executed\n",properties.deviceName,source,target,changed);
 vkDestroyCommandPool(device,command_pool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);vkDestroySampler(device,sampler,nullptr);vkDestroyImageView(device,sampled_view,nullptr);vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,image_memory,nullptr);vkUnmapMemory(device,buffer_memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,buffer_memory,nullptr);vkDestroyDevice(device,nullptr);
 if(messenger){const auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));CHECK(destroy);destroy(instance,messenger,nullptr);}
 vkDestroyInstance(instance,nullptr);
 const auto issues=validation_issues.load(std::memory_order_relaxed);std::printf("[STORAGE-VALIDATION] issues=%u callback=%u\n",issues,debug_utils);CHECK(issues==0);
 return 0;
 }catch(const std::exception& e){std::printf("[STORAGE-MIP-GPU] FAIL: %s\n",e.what());return 1;}}
