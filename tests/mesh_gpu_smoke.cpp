#include <vulkan/vulkan.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <string>
static void Check(VkResult r,const char* operation){if(r!=VK_SUCCESS){std::fprintf(stderr,"%s: VkResult %d\n",operation,int(r));throw std::runtime_error(operation);}}
#define VK_CHECK(call) Check(call,#call)
static uint32_t MemoryType(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags){
 VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);
 for(uint32_t i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags)return i;
 throw std::runtime_error("Required memory type unavailable");
}
static VkShaderModule Shader(VkDevice device,const char* path){
 std::ifstream f(path,std::ios::binary|std::ios::ate);const std::streamoff bytes=f.tellg();
 if(!f || bytes<20 || bytes>1024*1024 || bytes%4)throw std::runtime_error("Invalid SPIR-V file");
 std::vector<uint32_t> words(size_t(bytes)/4);f.seekg(0);
 if(!f.read(reinterpret_cast<char*>(words.data()),bytes))throw std::runtime_error("SPIR-V read failed");
 VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=size_t(bytes);info.pCode=words.data();
 VkShaderModule module;VK_CHECK(vkCreateShaderModule(device,&info,nullptr,&module));return module;
}
int main(int argc,char** argv){try{
 if(argc!=3 && argc!=4)throw std::runtime_error("Usage: mesh_gpu_smoke.exe mesh.spv fragment.spv");
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="psemu Mesh smoke";app.apiVersion=VK_API_VERSION_1_2;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
 VkInstance instance;VK_CHECK(vkCreateInstance(&ici,nullptr,&instance));
 uint32_t count=0;VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));std::vector<VkPhysicalDevice> devices(count);
 VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));VkPhysicalDevice gpu=VK_NULL_HANDLE;uint32_t family=0;
 for(auto candidate:devices){
  VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(candidate,&properties);
  if(properties.apiVersion<VK_API_VERSION_1_2)continue;
  uint32_t extensions=0;VK_CHECK(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extensions,nullptr));
  std::vector<VkExtensionProperties> available(extensions);VK_CHECK(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extensions,available.data()));
  bool mesh_extension=false;for(const auto& e:available)if(std::strcmp(e.extensionName,VK_EXT_MESH_SHADER_EXTENSION_NAME)==0)mesh_extension=true;
  if(!mesh_extension)continue;
  VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&mesh;vkGetPhysicalDeviceFeatures2(candidate,&features);
  if(!mesh.meshShader)continue;
  VkPhysicalDeviceMeshShaderPropertiesEXT limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
  VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&limits;vkGetPhysicalDeviceProperties2(candidate,&props);
  if(limits.maxMeshOutputVertices<216 || limits.maxMeshOutputPrimitives<210)continue;
  uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&queues,nullptr);std::vector<VkQueueFamilyProperties> q(queues);
  vkGetPhysicalDeviceQueueFamilyProperties(candidate,&queues,q.data());
  for(uint32_t i=0;i<queues;++i)if(q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){gpu=candidate;family=i;break;}
  if(gpu){std::printf("[MESH-GPU] device=%s\n",properties.deviceName);break;}
 }
 if(!gpu)throw std::runtime_error("No suitable Mesh GPU");
 const float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};mesh.meshShader=VK_TRUE;
 const char* extension=VK_EXT_MESH_SHADER_EXTENSION_NAME;VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&mesh;
 dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=1;dci.ppEnabledExtensionNames=&extension;
 VkDevice device;VK_CHECK(vkCreateDevice(gpu,&dci,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 const bool buffer_reads=argc==4 && std::string(argv[3])=="buffer-reads";
 const bool point_push=buffer_reads || (argc==4 && std::string(argv[3])=="point-push");
 const bool vertex_probe=argc==4 && std::string(argv[3])=="vertex-probe";
 if(argc==4 && !point_push && !vertex_probe) throw std::runtime_error("Unknown Mesh GPU test mode");
 auto draw=reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(device,"vkCmdDrawMeshTasksEXT"));if(!draw)throw std::runtime_error("Mesh draw entry unavailable");
 constexpr uint32_t width=32,height=32,bytes=width*height*4;
 VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.imageType=VK_IMAGE_TYPE_2D;image_info.format=VK_FORMAT_R8G8B8A8_UNORM;
 image_info.extent={width,height,1};image_info.mipLevels=1;image_info.arrayLayers=1;image_info.samples=VK_SAMPLE_COUNT_1_BIT;
 image_info.tiling=VK_IMAGE_TILING_OPTIMAL;image_info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
 VkImage image;VK_CHECK(vkCreateImage(device,&image_info,nullptr,&image));VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,image,&mr);
 VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=mr.size;alloc.memoryTypeIndex=MemoryType(gpu,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
 VkDeviceMemory image_memory;VK_CHECK(vkAllocateMemory(device,&alloc,nullptr,&image_memory));VK_CHECK(vkBindImageMemory(device,image,image_memory,0));
 VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=image_info.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageView view;VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
 VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 VkBuffer buffer;VK_CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer));vkGetBufferMemoryRequirements(device,buffer,&mr);
 alloc.allocationSize=mr.size;alloc.memoryTypeIndex=MemoryType(gpu,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 VkDeviceMemory buffer_memory;VK_CHECK(vkAllocateMemory(device,&alloc,nullptr,&buffer_memory));VK_CHECK(vkBindBufferMemory(device,buffer,buffer_memory,0));
 VkAttachmentDescription attachment{};attachment.format=image_info.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
 attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
 VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
 VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ri.attachmentCount=1;ri.pAttachments=&attachment;ri.subpassCount=1;ri.pSubpasses=&subpass;
 VkRenderPass render_pass;VK_CHECK(vkCreateRenderPass(device,&ri,nullptr,&render_pass));
 VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=render_pass;fi.attachmentCount=1;fi.pAttachments=&view;fi.width=width;fi.height=height;fi.layers=1;
 VkFramebuffer framebuffer;VK_CHECK(vkCreateFramebuffer(device,&fi,nullptr,&framebuffer));
 const auto mesh_shader=Shader(device,argv[1]),fragment_shader=Shader(device,argv[2]);
 VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
 stages[0].stage=vertex_probe ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_MESH_BIT_EXT;stages[0].module=mesh_shader;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment_shader;
 VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D scissor{{0,0},{width,height}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
 VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
 VkDescriptorSetLayout storage_layout=VK_NULL_HANDLE;VkDescriptorPool storage_pool=VK_NULL_HANDLE;
 VkDescriptorSet storage_set=VK_NULL_HANDLE;VkBuffer input_buffer=VK_NULL_HANDLE;VkDeviceMemory input_memory=VK_NULL_HANDLE;
 if(buffer_reads) {
  VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorCount=1;
  binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;binding.stageFlags=VK_SHADER_STAGE_MESH_BIT_EXT;
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount=1;layout_info.pBindings=&binding;VK_CHECK(vkCreateDescriptorSetLayout(device,&layout_info,nullptr,&storage_layout));
  VkBufferCreateInfo input_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};input_info.size=4096;input_info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VK_CHECK(vkCreateBuffer(device,&input_info,nullptr,&input_buffer));VkMemoryRequirements input_req;vkGetBufferMemoryRequirements(device,input_buffer,&input_req);
  VkMemoryAllocateInfo input_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};input_alloc.allocationSize=input_req.size;
  input_alloc.memoryTypeIndex=MemoryType(gpu,input_req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VK_CHECK(vkAllocateMemory(device,&input_alloc,nullptr,&input_memory));VK_CHECK(vkBindBufferMemory(device,input_buffer,input_memory,0));
  void* mapped;VK_CHECK(vkMapMemory(device,input_memory,0,4096,0,&mapped));
  for(uint32_t i=0;i<1024;++i) static_cast<uint32_t*>(mapped)[i]=0x2000+i;
  vkUnmapMemory(device,input_memory);
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets=1;pool_info.poolSizeCount=1;pool_info.pPoolSizes=&pool_size;VK_CHECK(vkCreateDescriptorPool(device,&pool_info,nullptr,&storage_pool));
  VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set_info.descriptorPool=storage_pool;
  set_info.descriptorSetCount=1;set_info.pSetLayouts=&storage_layout;VK_CHECK(vkAllocateDescriptorSets(device,&set_info,&storage_set));
  VkDescriptorBufferInfo buffer_info{input_buffer,0,4096};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet=storage_set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&buffer_info;
  vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 }
 VkPushConstantRange push_range{VK_SHADER_STAGE_MESH_BIT_EXT,0,24};
 VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};if(point_push){li.pushConstantRangeCount=1;li.pPushConstantRanges=&push_range;}if(buffer_reads){li.setLayoutCount=1;li.pSetLayouts=&storage_layout;}VkPipelineLayout layout;VK_CHECK(vkCreatePipelineLayout(device,&li,nullptr,&layout));
 VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&ms;pi.pColorBlendState=&cb;pi.layout=layout;pi.renderPass=render_pass;
 if(vertex_probe) {pi.pVertexInputState=&vertex_input;pi.pInputAssemblyState=&assembly;}
 VkPipeline pipeline;VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
 VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.queueFamilyIndex=family;ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool pool;VK_CHECK(vkCreateCommandPool(device,&ci,nullptr,&pool));
 VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
 VkCommandBuffer cmd;VK_CHECK(vkAllocateCommandBuffers(device,&cai,&cmd));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bool readbacks_passed=true;
 for(uint32_t point_case=0;point_case<(point_push?2u:1u);++point_case) {
 if(point_case!=0) VK_CHECK(vkResetCommandBuffer(cmd,0));
 VK_CHECK(vkBeginCommandBuffer(cmd,&begin));
 VkClearValue clear{};clear.color.float32[3]=1;VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=render_pass;rp.framebuffer=framebuffer;rp.renderArea=scissor;rp.clearValueCount=1;rp.pClearValues=&clear;
 vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
 if(buffer_reads) vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&storage_set,0,nullptr);
 if(point_push) {
  const uint32_t payload[6]={0x87654321u,point_case?2u:1u,point_case?41u:25u,point_case?43u:27u,point_case?0xabcdef11u:0xabcdef01u,0x12345678u};
  vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_MESH_BIT_EXT,0,sizeof(payload),payload);
 }
 if(vertex_probe) vkCmdDraw(cmd,3,1,0,0);else draw(cmd,1,1,1);vkCmdEndRenderPass(cmd);
 VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;ib.oldLayout=ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
 ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.image=image;ib.subresourceRange=vi.subresourceRange;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
 VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
 VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb.buffer=buffer;bb.size=bytes;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&bb,0,nullptr);VK_CHECK(vkEndCommandBuffer(cmd));
 VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;VK_CHECK(vkCreateFence(device,&fence_info,nullptr,&fence));
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,20000000000ull));
 void* pixels;VK_CHECK(vkMapMemory(device,buffer_memory,0,bytes,0,&pixels));auto* p=static_cast<unsigned char*>(pixels);uint32_t red=0;
 for(uint32_t i=0;i<width*height;++i)if((point_case==1 ? (p[i*4]<10 && p[i*4+1]>240) : (p[i*4]>240 && p[i*4+1]<10)) && p[i*4+2]<10)++red;
 vkUnmapMemory(device,buffer_memory);
 std::printf("[MESH-GPU] case=%u expected=%s correct_pixels=%u total=%u Astro=not-executed\n",point_case,point_case==1?"green":"red",red,width*height);
 if(vertex_probe) {
  std::printf("[VERTEX-GPU-PROBE] captured_vertex_shader=1 colored_pixels=%u total=%u game_frame=0\n",red,width*height);
  // The captured procedural shader forms the half-screen (-1,-1),(1,-1),(-1,1) triangle.
  readbacks_passed=readbacks_passed && red>=480 && red<=544;
 } else readbacks_passed=readbacks_passed && red>=1000;
 vkDestroyFence(device,fence,nullptr);
 }

 if(buffer_reads) {
  vkDestroyDescriptorPool(device,storage_pool,nullptr);vkDestroyDescriptorSetLayout(device,storage_layout,nullptr);
  vkDestroyBuffer(device,input_buffer,nullptr);vkFreeMemory(device,input_memory,nullptr);
 }
 vkDestroyCommandPool(device,pool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);
 vkDestroyShaderModule(device,mesh_shader,nullptr);vkDestroyShaderModule(device,fragment_shader,nullptr);vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,render_pass,nullptr);
 vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,image_memory,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,buffer_memory,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
 return readbacks_passed?0:1;
}catch(const std::exception& e){std::fprintf(stderr,"Mesh GPU smoke failed: %s\n",e.what());return 2;}}
