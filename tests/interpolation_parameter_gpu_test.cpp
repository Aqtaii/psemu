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

#include <cmath>

int main(int argc,char**argv){try{
 CHECK(argc==4);const unsigned which=unsigned(std::strtoul(argv[3],nullptr,10));CHECK(which<5);
 auto read=[](const char* p){std::ifstream f(p,std::ios::binary|std::ios::ate);CHECK(f);size_t n=size_t(f.tellg());CHECK(n && n%4==0);std::vector<uint32_t> v(n/4);f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);CHECK(f);return v;};
 auto vs=read(argv[1]),fs=read(argv[2]);
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
 CHECK(debug_utils && ici.enabledLayerCount==1);
 VkInstance instance;VKOK(vkCreateInstance(&ici,nullptr,&instance));VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
 if(debug_utils){const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
  if(!create)throw std::runtime_error("Debug utils entry unavailable");VKOK(create(instance,&debug_info,nullptr,&messenger));}

 uint32_t physical_count=0;VKOK(vkEnumeratePhysicalDevices(instance,&physical_count,nullptr));CHECK(physical_count);
 std::vector<VkPhysicalDevice> physicals(physical_count);VKOK(vkEnumeratePhysicalDevices(instance,&physical_count,physicals.data()));
 VkPhysicalDevice physical=physicals[0];VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
 uint32_t family_count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,nullptr);
 std::vector<VkQueueFamilyProperties> families(family_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,families.data());
 uint32_t family=0;while(family<family_count && (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)))++family;CHECK(family<family_count);
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;


 VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
 VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};supported.pNext=&bary;
 vkGetPhysicalDeviceFeatures2(physical,&supported);CHECK(bary.fragmentShaderBarycentric);
 const char* ext=VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
 dci.pNext=&bary;dci.enabledExtensionCount=1;dci.ppEnabledExtensionNames=&ext;
 VkDevice device;VKOK(vkCreateDevice(physical,&dci,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);
 auto alloc=[&](VkMemoryRequirements req,VkMemoryPropertyFlags flags){
  uint32_t i=0;while(i<mp.memoryTypeCount&&(!(req.memoryTypeBits&(1u<<i))||(mp.memoryTypes[i].propertyFlags&flags)!=flags))++i;
  CHECK(i<mp.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=i;
  VkDeviceMemory m;VKOK(vkAllocateMemory(device,&ai,nullptr,&m));return m;
 };
 constexpr uint32_t w=16,h=16;constexpr size_t bytes=w*h*16+128;
 VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=bytes;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 VkBuffer buffer;VKOK(vkCreateBuffer(device,&bc,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
 auto bm=alloc(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VKOK(vkBindBufferMemory(device,buffer,bm,0));
 void* mapped;VKOK(vkMapMemory(device,bm,0,bytes,0,&mapped));std::memset(mapped,0xa5,bytes);
 VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R32G32B32A32_SFLOAT;ic.extent={w,h,1};
 ic.mipLevels=ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
 VkImage image;VKOK(vkCreateImage(device,&ic,nullptr,&image));vkGetImageMemoryRequirements(device,image,&req);auto im=alloc(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKOK(vkBindImageMemory(device,image,im,0));
 VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vc.image=image;vc.viewType=VK_IMAGE_VIEW_TYPE_2D;vc.format=ic.format;vc.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageView view;VKOK(vkCreateImageView(device,&vc,nullptr,&view));
 VkAttachmentDescription attachment{};attachment.format=ic.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
 VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
 VkSubpassDependency dependencies[2]={{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0},
 {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,0}};
 VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rc.attachmentCount=1;rc.pAttachments=&attachment;rc.subpassCount=1;rc.pSubpasses=&sub;rc.dependencyCount=2;rc.pDependencies=dependencies;
 VkRenderPass pass;VKOK(vkCreateRenderPass(device,&rc,nullptr,&pass));
 VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fc.renderPass=pass;fc.attachmentCount=1;fc.pAttachments=&view;fc.width=w;fc.height=h;fc.layers=1;
 VkFramebuffer fb;VKOK(vkCreateFramebuffer(device,&fc,nullptr,&fb));
 auto module=[&](const auto& v){VkShaderModuleCreateInfo c{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};c.codeSize=v.size()*4;c.pCode=v.data();VkShaderModule m;VKOK(vkCreateShaderModule(device,&c,nullptr,&m));return m;};
 auto vertex=module(vs),fragment=module(fs);
 VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vertex,"main",nullptr},
 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fragment,"main",nullptr}};
 VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VkPipelineLayout layout;VKOK(vkCreatePipelineLayout(device,&lc,nullptr,&layout));
 VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport viewport{0,0,float(w),float(h),0,1};VkRect2D scissor{{0,0},{w,h}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState ca{};ca.colorWriteMask=15;
 VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&ca;
 VkGraphicsPipelineCreateInfo gc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gc.stageCount=2;gc.pStages=stages;gc.pVertexInputState=&vi;gc.pInputAssemblyState=&ia;gc.pViewportState=&vp;gc.pRasterizationState=&raster;gc.pMultisampleState=&ms;gc.pColorBlendState=&blend;gc.layout=layout;gc.renderPass=pass;
 VkPipeline pipeline;VKOK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gc,nullptr,&pipeline));
 VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;
 VkCommandPool pool;VKOK(vkCreateCommandPool(device,&pc,nullptr,&pool));
 VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ac.commandPool=pool;ac.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ac.commandBufferCount=1;
 VkCommandBuffer cmd;VKOK(vkAllocateCommandBuffers(device,&ac,&cmd));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(cmd,&begin));
 VkClearValue clear{};VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.framebuffer=fb;rp.renderArea=scissor;rp.clearValueCount=1;rp.pClearValues=&clear;
 vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
 VkBufferImageCopy copy{};copy.bufferOffset=64;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
 VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
 VKOK(vkEndCommandBuffer(cmd));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;VKOK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
 const auto* data=reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped)+64);
 for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
  const float u=(x+0.5f)/(2*w),v=(y+0.5f)/(2*h),b0=1-u-v;
  const float interp=which==3?b0*.125f+u*.375f+v*.875f:(b0*.125f+u*.375f/2+v*.875f/4)/(b0+u/2+v/4);
  const float expected[4]={which==4?.125f:which==1?.375f:.25f,which==4?0:which==1?.875f:.75f,which==4?0:(which==2||which==3)?interp:.125f,1};
  for(unsigned c=0;c<4;c++)if(!std::isfinite(data[(y*w+x)*4+c]) || std::abs(data[(y*w+x)*4+c]-expected[c])>0.00001f){
   std::fprintf(stderr,"pixel(%u,%u) channel%u actual=%g expected=%g case=%u\n",x,y,c,data[(y*w+x)*4+c],expected[c],which);throw std::runtime_error("Wrong interpolation value");
  }
 }
 for(size_t i=0;i<64;i++)CHECK(static_cast<uint8_t*>(mapped)[i]==0xa5 && static_cast<uint8_t*>(mapped)[bytes-1-i]==0xa5);
 vkDestroyCommandPool(device,pool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyShaderModule(device,vertex,nullptr);vkDestroyShaderModule(device,fragment,nullptr);vkDestroyFramebuffer(device,fb,nullptr);vkDestroyRenderPass(device,pass,nullptr);vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,im,nullptr);vkUnmapMemory(device,bm);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyDevice(device,nullptr);
 if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));CHECK(destroy);destroy(instance,messenger,nullptr);}
 vkDestroyInstance(instance,nullptr);CHECK(validation_issues.load()==0);
 printf("PASS native interpolation case=%u pixels=%u guards=preserved barycentric=enabled validation_issues=0 device=%s\n",which,w*h,properties.deviceName);return 0;
}catch(const std::exception&e){fprintf(stderr,"Interpolation GPU failure: %s\n",e.what());return 1;}}
