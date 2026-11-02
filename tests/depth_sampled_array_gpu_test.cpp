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
 std::fprintf(stderr,"[DEPTH-ARRAY-VALIDATION] %s\n",data && data->pMessage ? data->pMessage : "missing diagnostic");return VK_FALSE;
}

int main(int argc,char**argv){try{
 CHECK(argc==2);
 std::ifstream file(argv[1],std::ios::binary|std::ios::ate);CHECK(file);
 size_t bytes=size_t(file.tellg());CHECK(bytes && bytes%4==0);
 std::vector<uint32_t> code(bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);CHECK(file);
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
 uint32_t family=0;while(family<family_count && (!(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)))++family;CHECK(family<family_count);
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;

 VkDevice device;VKOK(vkCreateDevice(physical,&dci,nullptr,&device));
 VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);
 auto alloc=[&](VkMemoryRequirements req,VkMemoryPropertyFlags flags){
  uint32_t type=0;while(type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (mp.memoryTypes[type].propertyFlags&flags)!=flags))++type;
  CHECK(type<mp.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
  VkDeviceMemory mem;VKOK(vkAllocateMemory(device,&ai,nullptr,&mem));return mem;
 };
 constexpr uint32_t w=64,h=32,layers=3,n=w*h*layers;
 constexpr size_t output_offset=n*2+256,total=output_offset+n*4+256;
 VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=total;bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
 VkBuffer buffer;VKOK(vkCreateBuffer(device,&bc,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
 auto bm=alloc(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VKOK(vkBindBufferMemory(device,buffer,bm,0));
 void* mapped;VKOK(vkMapMemory(device,bm,0,total,0,&mapped));std::memset(mapped,0xa5,total);
 auto* input=static_cast<uint16_t*>(mapped);auto* output=reinterpret_cast<float*>(static_cast<uint8_t*>(mapped)+output_offset);
 for(uint32_t l=0;l<layers;l++)for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) input[(l*h+y)*w+x]=uint16_t(8192+l*24576+((x+7*y)&1023));
 std::vector<uint8_t> initial(static_cast<uint8_t*>(mapped),static_cast<uint8_t*>(mapped)+total);
 VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_D16_UNORM,&fp);
 CHECK((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))==(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
 VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_D16_UNORM;ic.extent={w,h,1};
 ic.mipLevels=1;ic.arrayLayers=layers;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
 ic.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 VkImage image;VKOK(vkCreateImage(device,&ic,nullptr,&image));vkGetImageMemoryRequirements(device,image,&req);auto im=alloc(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKOK(vkBindImageMemory(device,image,im,0));
 VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_NEAREST;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
 sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sc.compareEnable=VK_TRUE;sc.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
 VkSampler sampler;VKOK(vkCreateSampler(device,&sc,nullptr,&sampler));
 VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
 VkDescriptorSetLayoutCreateInfo slc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};slc.bindingCount=2;slc.pBindings=bindings;
 VkDescriptorSetLayout sl;VKOK(vkCreateDescriptorSetLayout(device,&slc,nullptr,&sl));
 VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
 VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=2;pc.pPoolSizes=sizes;
 VkDescriptorPool pool;VKOK(vkCreateDescriptorPool(device,&pc,nullptr,&pool));
 VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=pool;sa.descriptorSetCount=1;sa.pSetLayouts=&sl;
 VkDescriptorSet set;VKOK(vkAllocateDescriptorSets(device,&sa,&set));
 VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
 VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plc.setLayoutCount=1;plc.pSetLayouts=&sl;plc.pushConstantRangeCount=1;plc.pPushConstantRanges=&push;
 VkPipelineLayout pl;VKOK(vkCreatePipelineLayout(device,&plc,nullptr,&pl));
 VkShaderModuleCreateInfo smc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smc.codeSize=bytes;smc.pCode=code.data();
 VkShaderModule shader;VKOK(vkCreateShaderModule(device,&smc,nullptr,&shader));
 VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=pl;cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
 VkPipeline pipeline;VKOK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline));
 VkCommandPoolCreateInfo cpc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpc.queueFamilyIndex=family;cpc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
 VkCommandPool commands;VKOK(vkCreateCommandPool(device,&cpc,nullptr,&commands));
 VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commands;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
 VkCommandBuffer command;VKOK(vkAllocateCommandBuffers(device,&ca,&command));
 for(uint32_t base=0;base<layers;base++){
  VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};usage.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vc.pNext=&usage;vc.image=image;vc.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;vc.format=VK_FORMAT_D16_UNORM;
  vc.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R};
  vc.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,base,layers-base};VkImageView view;VKOK(vkCreateImageView(device,&vc,nullptr,&view));
  VkDescriptorImageInfo di{sampler,view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo db{buffer,output_offset,n*4};
  VkWriteDescriptorSet writes[2]{};for(auto&v:writes)v.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet=set;writes[0].dstBinding=0;writes[0].descriptorCount=1;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&di;
  writes[1].dstSet=set;writes[1].dstBinding=1;writes[1].descriptorCount=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&db;
  vkUpdateDescriptorSets(device,2,writes,0,nullptr);
  VKOK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(command,&begin));
  if(base==0){
   VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.image=image;ib.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,layers};ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
   vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
   VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,layers};copy.imageExtent={w,h,1};
   vkCmdCopyBufferToImage(command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
   ib.oldLayout=ib.newLayout;ib.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
   vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
  }
  vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);
  uint32_t constants[2]={base,layers-base};vkCmdPushConstants(command,pl,VK_SHADER_STAGE_COMPUTE_BIT,0,8,constants);
  vkCmdDispatch(command,w/8,h/8,layers-base);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
  VKOK(vkEndCommandBuffer(command));VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};sub.commandBufferCount=1;sub.pCommandBuffers=&command;VKOK(vkQueueSubmit(queue,1,&sub,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
  for(uint32_t l=0;l<layers-base;l++)for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
   const float ref=(x&1)?0.75f:0.25f;const bool expected=ref<=float(input[((base+l)*h+y)*w+x])/65535.f;
   CHECK(output[(l*h+y)*w+x]==float(expected));
  }
  for(size_t i=0;i<output_offset;i++)CHECK(static_cast<uint8_t*>(mapped)[i]==initial[i]);
  for(size_t i=output_offset+n*4;i<total;i++)CHECK(static_cast<uint8_t*>(mapped)[i]==initial[i]);
  vkDestroyImageView(device,view,nullptr);
 }
 vkDestroyCommandPool(device,commands,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);vkDestroyPipelineLayout(device,pl,nullptr);
 vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyDescriptorSetLayout(device,sl,nullptr);vkDestroySampler(device,sampler,nullptr);
 vkDestroyImage(device,image,nullptr);vkFreeMemory(device,im,nullptr);vkUnmapMemory(device,bm);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyDevice(device,nullptr);
 if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));CHECK(destroy);destroy(instance,messenger,nullptr);}
 vkDestroyInstance(instance,nullptr);CHECK(validation_issues.load()==0);
 std::printf("PASS D16 three-layer shadow compare, base layers 0/1/2, 12288 samples, input/exterior guards, validation_issues=0 device=%s\n",properties.deviceName);
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL depth sampled array GPU: %s\n",e.what());return 1;}}
