#define NOMINMAX
#include <vulkan/vulkan.h>
#include "graphics/host_gpu/renderer/htileZRange.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
using namespace Libs::Graphics;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
#define VKOK(x) CHECK((x)==VK_SUCCESS)
static std::atomic_uint validation_issues{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
 VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* d,void*){
 ++validation_issues;std::fprintf(stderr,"[HTILE-VALIDATION] %s\n",d->pMessage);return VK_FALSE;
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
  VkPhysicalDeviceFeatures supported{};vkGetPhysicalDeviceFeatures(physical,&supported);
  CHECK(supported.shaderStorageImageReadWithoutFormat && supported.shaderStorageImageWriteWithoutFormat &&
        supported.shaderStorageImageExtendedFormats);
  VkPhysicalDeviceFeatures features{};features.shaderStorageImageReadWithoutFormat=VK_TRUE;
  features.shaderStorageImageWriteWithoutFormat=VK_TRUE;features.shaderStorageImageExtendedFormats=VK_TRUE;
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
  Vulkan v;constexpr uint32_t w=1920,h=1080,ow=256,oh=144;constexpr size_t samples=size_t(w)*h;
  const VkImageAspectFlags both=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
  CHECK(v.props.limits.minStorageBufferOffsetAlignment<=256);
  const auto depth=v.MakeImage(w,h,VK_FORMAT_D32_SFLOAT_S8_UINT,
   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  const auto output=v.MakeImage(ow,oh,VK_FORMAT_R32G32_SFLOAT,
   VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  auto upload=v.MakeBuffer(samples*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  auto readback=v.MakeBuffer(samples*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  auto stencil=v.MakeBuffer(samples,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  auto result=v.MakeBuffer(size_t(ow)*oh*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  std::vector<float> initial(samples);
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
   initial[size_t(y)*w+x]=float((x/8*7+y/8*11)%101)/101.f+float((x+y)%7)*.0001f;
  initial[0]=0;initial[1]=1;std::memcpy(upload.map+256,initial.data(),samples*4);
  v.Begin();
  v.Transition(depth,both,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkClearDepthStencilValue clear{1,0x5a};VkImageSubresourceRange range{both,0,1,0,1};
  vkCmdClearDepthStencilImage(v.command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
  VkMemoryBarrier ordered{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ordered.srcAccessMask=ordered.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&ordered,0,nullptr,0,nullptr);
  VkBufferImageCopy copy{};copy.bufferOffset=256;copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.imageExtent={w,h,1};
  vkCmdCopyBufferToImage(v.command,upload.handle,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  v.Transition(depth,both,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  // Match the production read-only transaction, including restoration of both aspect layouts.
  v.Transition(depth,both,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyImageToBuffer(v.command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
  auto scopy=copy;scopy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
  vkCmdCopyImageToBuffer(v.command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stencil.handle,1,&scopy);
  v.Transition(depth,both,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
  v.Submit();CHECK(!std::memcmp(initial.data(),readback.map+256,samples*4));
  for(size_t i=0;i<samples;++i)CHECK(stencil.map[256+i]==0x5a);
  HtileZRangeLayout hl;CHECK(MakeHtileZRangeLayout(w,h,2,0x30000,0xc0000,&hl));
  std::vector<uint32_t>packed;CHECK(BuildHtileZRangeSnapshot(
    {reinterpret_cast<const float*>(readback.map+256),samples},hl,&packed));
  auto meta=v.MakeBuffer(hl.view_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  auto cb=v.MakeBuffer(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  std::memcpy(meta.map+256,packed.data(),hl.view_bytes);std::memset(cb.map+256,0,16);
  reinterpret_cast<uint32_t*>(cb.map+256)[0]=2048;
  const std::vector<uint8_t> meta_before(meta.map,meta.map+meta.bytes),cb_before(cb.map,cb.map+cb.bytes);
  VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                                          {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=2;sl.pBindings=bindings;
  VKOK(vkCreateDescriptorSetLayout(v.device,&sl,nullptr,&v.sl));
  VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
  VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=2;pc.pPoolSizes=sizes;
  VKOK(vkCreateDescriptorPool(v.device,&pc,nullptr,&v.pool));
  VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=v.pool;sa.descriptorSetCount=1;sa.pSetLayouts=&v.sl;
  VkDescriptorSet set{};VKOK(vkAllocateDescriptorSets(v.device,&sa,&set));
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,64};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount=1;pl.pSetLayouts=&v.sl;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
  VKOK(vkCreatePipelineLayout(v.device,&pl,nullptr,&v.layout));
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=bytes;sm.pCode=code.data();
  VKOK(vkCreateShaderModule(v.device,&sm,nullptr,&v.shader));
  VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=v.layout;
  cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,v.shader,"main",nullptr};
  VKOK(vkCreateComputePipelines(v.device,VK_NULL_HANDLE,1,&cp,nullptr,&v.pipeline));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=output.handle;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R32G32_SFLOAT;
  vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view{};VKOK(vkCreateImageView(v.device,&vi,nullptr,&view));v.views.push_back(view);
  VkDescriptorBufferInfo bi[2]={{cb.handle,256,16},{meta.handle,256,hl.view_bytes}};
  VkDescriptorImageInfo ii{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};for(auto& wr:writes)wr.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet=set;writes[0].dstBinding=0;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[0].descriptorCount=2;writes[0].pBufferInfo=bi;
  writes[1].dstSet=set;writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[1].descriptorCount=1;writes[1].pImageInfo=&ii;
  vkUpdateDescriptorSets(v.device,2,writes,0,nullptr);
  v.Begin();v.Transition(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkClearColorValue sentinel{};sentinel.float32[0]=-7;sentinel.float32[1]=-11;
  VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  vkCmdClearColorImage(v.command,output.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&sentinel,1,&color_range);
  v.Transition(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
   VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  vkCmdBindPipeline(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.pipeline);
  vkCmdBindDescriptorSets(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.layout,0,1,&set,0,nullptr);
  const std::array<uint32_t,16> constants{};vkCmdPushConstants(v.command,v.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,64,constants.data());
  vkCmdDispatch(v.command,30,17,1);
  v.Transition(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy oc{};oc.bufferOffset=256;oc.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};oc.imageExtent={ow,oh,1};
  vkCmdCopyImageToBuffer(v.command,output.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,result.handle,1,&oc);
  vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
  v.Submit();
  const auto* z=reinterpret_cast<const float*>(result.map+256);
  for(uint32_t y=0;y<oh;++y)for(uint32_t x=0;x<ow;++x){
   const size_t i=(size_t(y)*ow+x)*2;
   if(x>=240 || y>=136){CHECK(z[i]==-7 && z[i+1]==-11);continue;}
   const auto word=packed[HtileZRangeByteOffset(hl,x,y)/4];const float scale=1.f/16383.f;
   const float lo=float((word>>4)&16383)*scale,hi=float(word>>18)*scale;
   if(std::fabs(z[i]-lo)>1.e-6f || std::fabs(z[i+1]-hi)>1.e-6f){
    std::fprintf(stderr,"mismatch xy=%u,%u got=%g,%g expected=%g,%g word=%08x\n",x,y,z[i],z[i+1],lo,hi,word);
    CHECK(false);
   }
  }
  CHECK(!std::memcmp(meta_before.data(),meta.map,meta.bytes) && !std::memcmp(cb_before.data(),cb.map,cb.bytes));
  for(const auto& b:v.buffers)Guards(b);
  // Re-read the real native image after the original consumer has completed.
  v.Begin();v.Transition(depth,both,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyImageToBuffer(v.command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
  vkCmdCopyImageToBuffer(v.command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stencil.handle,1,&scopy);
  v.Transition(depth,both,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);v.Submit();
  CHECK(!std::memcmp(initial.data(),readback.map+256,samples*4));
  for(size_t i=0;i<samples;++i)CHECK(stencil.map[256+i]==0x5a);
  for(const auto& b:v.buffers)Guards(b);
  std::printf("PASS HTile Vulkan: shader_bytes=%zu, 32640 Z-range pairs, 2073600 D32S8 depth/stencil samples preserved, padding/exterior guards device=%s\n",bytes,v.props.deviceName);
 }
 CHECK(validation_issues.load()==0);puts("PASS active Vulkan validation: 0 warnings/errors");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL HTile Vulkan: %s\n",e.what());return 1;}}
