#define NOMINMAX
#include <vulkan/vulkan.h>
#include <cmath>
#include <bit>
#include <array>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
#define VKOK(x) CHECK((x)==VK_SUCCESS)
static std::atomic_uint validation_issues{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
 VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* d,void*){
 ++validation_issues;std::fprintf(stderr,"[STENCIL-VALIDATION] %s\n",d->pMessage);return VK_FALSE;
}
struct Buffer {VkBuffer handle;VkDeviceMemory memory;uint8_t* map;size_t bytes;};
struct Image {VkImage handle;VkDeviceMemory memory;};
struct Vulkan {
 VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};VkPhysicalDevice physical{};
 VkDevice device{};VkQueue queue{};uint32_t family{};VkCommandPool commands{};VkCommandBuffer command{};
 VkPhysicalDeviceMemoryProperties memory{};VkPhysicalDeviceProperties props{};
 std::vector<Buffer> buffers;std::vector<Image>images;std::vector<VkImageView>views;
 VkDescriptorSetLayout sl{};VkDescriptorPool pool{};VkPipelineLayout layout{};VkShaderModule shader{};VkPipeline pipeline{};
 Vulkan(){
  uint32_t n=0;VKOK(vkEnumerateInstanceLayerProperties(&n,nullptr));std::vector<VkLayerProperties> layers(n);
  VKOK(vkEnumerateInstanceLayerProperties(&n,layers.data()));bool found=false;
  for(const auto& l:layers)if(!std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation"))found=true;
  CHECK(found);
  const char* layer="VK_LAYER_KHRONOS_validation";const char* ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  debug.pfnUserCallback=Validation;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.pNext=&debug;
  ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&ext;
  VKOK(vkCreateInstance(&ic,nullptr,&instance));
  auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
  CHECK(create);VKOK(create(instance,&debug,nullptr,&messenger));
  VKOK(vkEnumeratePhysicalDevices(instance,&n,nullptr));CHECK(n);std::vector<VkPhysicalDevice> physicals(n);
  VKOK(vkEnumeratePhysicalDevices(instance,&n,physicals.data()));physical=physicals[0];
  vkGetPhysicalDeviceProperties(physical,&props);vkGetPhysicalDeviceMemoryProperties(physical,&memory);
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
  while(family<n && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;CHECK(family<n);
  VkPhysicalDeviceFeatures features{}; // This sampled-image fixture needs no optional storage-image features.
  float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.pEnabledFeatures=&features;
  VKOK(vkCreateDevice(physical,&dc,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VKOK(vkCreateCommandPool(device,&pc,nullptr,&commands));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commands;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
  VKOK(vkAllocateCommandBuffers(device,&ca,&command));
 }
 VkDeviceMemory Allocate(VkMemoryRequirements r,VkMemoryPropertyFlags flags){
  uint32_t type=0;while(type<memory.memoryTypeCount && (!(r.memoryTypeBits&(1u<<type)) ||
   (memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;CHECK(type<memory.memoryTypeCount);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=type;
  VkDeviceMemory result{};VKOK(vkAllocateMemory(device,&ai,nullptr,&result));return result;
 }
 Buffer MakeBuffer(size_t payload,VkBufferUsageFlags usage){
  Buffer b{};b.bytes=payload+512;VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=b.bytes;bc.usage=usage;
  VKOK(vkCreateBuffer(device,&bc,nullptr,&b.handle));VkMemoryRequirements r{};vkGetBufferMemoryRequirements(device,b.handle,&r);
  b.memory=Allocate(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKOK(vkBindBufferMemory(device,b.handle,b.memory,0));void* map{};VKOK(vkMapMemory(device,b.memory,0,b.bytes,0,&map));
  b.map=static_cast<uint8_t*>(map);std::memset(b.map,0xa5,b.bytes);buffers.push_back(b);return b;
 }
 Image MakeImage(uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage){
  VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;ic.format=format;ic.extent={w,h,1};
  ic.mipLevels=ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=usage;
  Image i{};VKOK(vkCreateImage(device,&ic,nullptr,&i.handle));VkMemoryRequirements r{};vkGetImageMemoryRequirements(device,i.handle,&r);
  i.memory=Allocate(r,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKOK(vkBindImageMemory(device,i.handle,i.memory,0));images.push_back(i);return i;
 }
 void Begin(){VKOK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(command,&bi));}
 void Submit(){
  VKOK(vkEndCommandBuffer(command));VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&command;
  VKOK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
 }
 void Transition(Image i,VkImageAspectFlags aspects,VkImageLayout from,VkImageLayout to,
                 VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags before,VkPipelineStageFlags after){
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=i.handle;b.subresourceRange={aspects,0,1,0,1};
  b.oldLayout=from;b.newLayout=to;b.srcAccessMask=src;b.dstAccessMask=dst;
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  vkCmdPipelineBarrier(command,before,after,0,0,nullptr,0,nullptr,1,&b);
 }
 ~Vulkan(){
  if(device){
   vkDeviceWaitIdle(device);if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(shader)vkDestroyShaderModule(device,shader,nullptr);
   if(layout)vkDestroyPipelineLayout(device,layout,nullptr);if(pool)vkDestroyDescriptorPool(device,pool,nullptr);if(sl)vkDestroyDescriptorSetLayout(device,sl,nullptr);
   for(auto v:views)vkDestroyImageView(device,v,nullptr);
   vkDestroyCommandPool(device,commands,nullptr);
   for(auto i:images){vkDestroyImage(device,i.handle,nullptr);vkFreeMemory(device,i.memory,nullptr);}
   for(auto b:buffers){vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.handle,nullptr);vkFreeMemory(device,b.memory,nullptr);}
   vkDestroyDevice(device,nullptr);
  }
  if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(instance,messenger,nullptr);}
  if(instance)vkDestroyInstance(instance,nullptr);
 }
};
static void Guards(const Buffer& b){for(size_t i=0;i<256;++i)CHECK(b.map[i]==0xa5 && b.map[b.bytes-1-i]==0xa5);}
int main(int argc,char** argv){try{
 CHECK(argc==2);std::ifstream f(argv[1],std::ios::binary|std::ios::ate);CHECK(f);
 const size_t bytes=size_t(f.tellg());CHECK(bytes && bytes%4==0);std::vector<uint32_t>code(bytes/4);
 f.seekg(0);f.read(reinterpret_cast<char*>(code.data()),bytes);CHECK(f);
 {
  Vulkan v;constexpr uint32_t w=1920,h=1080;constexpr size_t n=size_t(w)*h;
  CHECK(v.props.limits.minStorageBufferOffsetAlignment<=256);
  const VkImageAspectFlags both=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
  auto image=v.MakeImage(w,h,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
    VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  auto depth_in=v.MakeBuffer(n*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  auto stencil_in=v.MakeBuffer(n,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  constexpr uint32_t depth_pitch=w+32,stencil_pitch=w+64;
  constexpr size_t depth_offset=256,stencil_offset=depth_offset+size_t(depth_pitch)*h*4+128;
  auto planes=v.MakeBuffer(stencil_offset-256+size_t(stencil_pitch)*h,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  auto result=v.MakeBuffer(n*20,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  auto* depth=reinterpret_cast<float*>(depth_in.map+256);
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
   const size_t i=size_t(y)*w+x;depth[i]=.125f+float((x+3*y)&31)/64.f;
   stencil_in.map[256+i]=uint8_t((x*13+y*7)%251);
  }
  v.Begin();
  v.Transition(image,both,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy dcopy{};dcopy.bufferOffset=256;dcopy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};dcopy.imageExtent={w,h,1};
  auto scopy=dcopy;scopy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
  vkCmdCopyBufferToImage(v.command,depth_in.handle,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&dcopy);
  vkCmdCopyBufferToImage(v.command,stencil_in.handle,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&scopy);
  v.Transition(image,both,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  v.Submit();
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;
  si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler{};VKOK(vkCreateSampler(v.device,&si,nullptr,&sampler));
  VkImageView views[2]{};
  for(uint32_t i=0;i<2;++i){
   VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};usage.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
   VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.pNext=&usage;vi.image=image.handle;vi.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
   vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.subresourceRange={VkImageAspectFlags(i==0?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT),0,1,0,1};
   if(i==0)vi.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R};
   VKOK(vkCreateImageView(v.device,&vi,nullptr,&views[i]));v.views.push_back(views[i]);
  }
  VkDescriptorSetLayoutBinding bindings[3]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
   {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
   {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=3;sl.pBindings=bindings;
  VKOK(vkCreateDescriptorSetLayout(v.device,&sl,nullptr,&v.sl));
  VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
  VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=2;pc.pPoolSizes=sizes;
  VKOK(vkCreateDescriptorPool(v.device,&pc,nullptr,&v.pool));
  VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=v.pool;sa.descriptorSetCount=1;sa.pSetLayouts=&v.sl;
  VkDescriptorSet set{};VKOK(vkAllocateDescriptorSets(v.device,&sa,&set));
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&v.sl;
  VKOK(vkCreatePipelineLayout(v.device,&pl,nullptr,&v.layout));
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=bytes;sm.pCode=code.data();
  VKOK(vkCreateShaderModule(v.device,&sm,nullptr,&v.shader));
  VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=v.layout;
  cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,v.shader,"main",nullptr};
  VKOK(vkCreateComputePipelines(v.device,VK_NULL_HANDLE,1,&cp,nullptr,&v.pipeline));
  VkDescriptorImageInfo ii[2]={{sampler,views[0],VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                              {sampler,views[1],VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
  VkDescriptorBufferInfo bi{result.handle,256,n*20};
  VkWriteDescriptorSet writes[3]{};
  for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;}
  for(uint32_t i=0;i<2;++i){writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&ii[i];}
  writes[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[2].pBufferInfo=&bi;
  vkUpdateDescriptorSets(v.device,3,writes,0,nullptr);
  v.Begin();vkCmdBindPipeline(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.pipeline);
  vkCmdBindDescriptorSets(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.layout,0,1,&set,0,nullptr);vkCmdDispatch(v.command,w/8,h/8,1);
  v.Transition(image,both,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy plane_copies[]={dcopy,scopy};
  plane_copies[0].bufferOffset=depth_offset;
  plane_copies[0].bufferRowLength=depth_pitch;
  plane_copies[1].bufferOffset=stencil_offset;
  plane_copies[1].bufferRowLength=stencil_pitch;
  vkCmdCopyImageToBuffer(v.command,image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,planes.handle,2,plane_copies);
  v.Transition(image,both,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);v.Submit();
  const auto* words=reinterpret_cast<const uint32_t*>(result.map+256);
  for(size_t i=0;i<n;++i){
   for(size_t c=0;c<4;++c)CHECK(words[i*5+c]==stencil_in.map[256+i]);
   CHECK(words[i*5+4]==std::bit_cast<uint32_t>(depth[i]));
  }
  for(uint32_t y=0;y<h;++y){
   const size_t depth_row=depth_offset+size_t(y)*depth_pitch*4;
   const size_t stencil_row=stencil_offset+size_t(y)*stencil_pitch;
   CHECK(!std::memcmp(depth+size_t(y)*w,planes.map+depth_row,w*4));
   CHECK(!std::memcmp(stencil_in.map+256+size_t(y)*w,planes.map+stencil_row,w));
   for(uint32_t i=w*4;i<depth_pitch*4;++i)CHECK(planes.map[depth_row+i]==0xa5);
   for(uint32_t i=w;i<stencil_pitch;++i)CHECK(planes.map[stencil_row+i]==0xa5);
  }
  for(size_t i=stencil_offset-128;i<stencil_offset;++i)CHECK(planes.map[i]==0xa5);
  for(const auto& b:v.buffers)Guards(b);
  vkDestroySampler(v.device,sampler,nullptr);
  std::printf("PASS S8 native sampled view: 2073600 spatial uint texels, RRRR swizzle, simultaneous depth sampling, batched D32/S8 readback, row padding and exterior guards device=%s\n",v.props.deviceName);
 }
 CHECK(validation_issues.load()==0);puts("PASS active Vulkan validation: 0 warnings/errors");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL stencil sampled Vulkan: %s\n",e.what());return 1;}}
