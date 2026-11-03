// Extracted production preparation loops, with a deliberately late owner join.
// CPU ordered-command and real Vulkan backends expose writes to retired buffers.
#define NOMINMAX
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
#define VKOK(x) CHECK((x)==VK_SUCCESS)
static std::atomic_uint validation_issues{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
 VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* d,void*) {
 ++validation_issues;std::fprintf(stderr,"[BUFFER-REBIND-VALIDATION] %s\n",d->pMessage);return VK_FALSE;
}
struct Allocation {
 VkBuffer buffer{};VkDeviceMemory memory{};size_t payload=0;
 std::vector<uint8_t> cpu,expected;uint8_t* map=nullptr;
};
struct Binding {std::shared_ptr<Allocation> owner;size_t offset=256;};
struct Device {
 bool gpu;VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};
 VkDebugUtilsMessengerEXT messenger{};VkQueue queue{};uint32_t family=0;
 VkPhysicalDeviceMemoryProperties memory{};VkCommandPool commands{};VkCommandBuffer command{};
 VkDescriptorSetLayout set_layout{};VkDescriptorPool descriptors{};VkPipelineLayout layout{};
 VkShaderModule shader{};VkPipeline pipeline{};
 std::vector<std::shared_ptr<Allocation>> allocations;
 std::vector<std::function<void()>> pending;
 explicit Device(bool enabled,const char* shader_path):gpu(enabled) {
  if(!gpu)return;
  uint32_t n=0;VKOK(vkEnumerateInstanceLayerProperties(&n,nullptr));
  std::vector<VkLayerProperties> layers(n);VKOK(vkEnumerateInstanceLayerProperties(&n,layers.data()));
  CHECK(std::any_of(layers.begin(),layers.end(),[](auto& l){return !std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation");}));
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
  VKOK(vkEnumeratePhysicalDevices(instance,&n,nullptr));CHECK(n);
  std::vector<VkPhysicalDevice> devices(n);VKOK(vkEnumeratePhysicalDevices(instance,&n,devices.data()));physical=devices[0];
  vkGetPhysicalDeviceMemoryProperties(physical,&memory);
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
  while(family<n && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;CHECK(family<n);
  const float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
  VKOK(vkCreateDevice(physical,&dc,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;
  VKOK(vkCreateCommandPool(device,&pc,nullptr,&commands));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool=commands;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
  VKOK(vkAllocateCommandBuffers(device,&ca,&command));
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sc.bindingCount=1;sc.pBindings=&binding;
  VKOK(vkCreateDescriptorSetLayout(device,&sc,nullptr,&set_layout));
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lc.setLayoutCount=1;lc.pSetLayouts=&set_layout;
  VKOK(vkCreatePipelineLayout(device,&lc,nullptr,&layout));
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
  VKOK(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors));
  std::ifstream file(shader_path,std::ios::binary|std::ios::ate);CHECK(file);
  const auto bytes=static_cast<size_t>(file.tellg());CHECK(bytes && bytes%4==0);
  std::vector<uint32_t> words(bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),bytes);CHECK(file.good());
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=bytes;sm.pCode=words.data();
  VKOK(vkCreateShaderModule(device,&sm,nullptr,&shader));
  VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};cp.layout=layout;
  VKOK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKOK(vkBeginCommandBuffer(command,&bi));
 }
 std::shared_ptr<Allocation> Make(size_t payload,uint8_t fill) {
  auto a=std::make_shared<Allocation>();a->payload=payload;a->expected.assign(payload+256,fill);
  if(gpu) {
   VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=a->expected.size();
   bc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VKOK(vkCreateBuffer(device,&bc,nullptr,&a->buffer));VkMemoryRequirements r{};vkGetBufferMemoryRequirements(device,a->buffer,&r);
   const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(r.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;
   CHECK(type<memory.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=type;
   VKOK(vkAllocateMemory(device,&ai,nullptr,&a->memory));VKOK(vkBindBufferMemory(device,a->buffer,a->memory,0));
   void* map{};VKOK(vkMapMemory(device,a->memory,0,a->expected.size(),0,&map));a->map=static_cast<uint8_t*>(map);
  } else {a->cpu.resize(a->expected.size());a->map=a->cpu.data();}
  std::memcpy(a->map,a->expected.data(),a->expected.size());allocations.push_back(a);return a;
 }
 void Barrier(VkAccessFlags before,VkAccessFlags after) {
  if(!gpu)return;
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=before;b.dstAccessMask=after;
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);
 }
 void Copy(const Binding& from,const Binding& to) {
  CHECK(from.owner->payload+1024<=to.owner->payload);
  std::memcpy(to.owner->expected.data()+1024,from.owner->expected.data(),from.owner->payload);
  if(gpu) {
   Barrier(VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
   VkBufferCopy copy{0,1024,from.owner->payload};vkCmdCopyBuffer(command,from.owner->buffer,to.owner->buffer,1,&copy);
   Barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
  } else pending.push_back([from,to]{std::memcpy(to.owner->map+1024,from.owner->map,from.owner->payload);});
 }
 void Write(const Binding& target) {
  for(uint32_t i=0;i<32;++i){uint32_t v=0x71100000u+i;std::memcpy(target.owner->expected.data()+target.offset+i*4,&v,4);}
  if(gpu) {
   VkDescriptorSet set{};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
   ai.descriptorPool=descriptors;ai.descriptorSetCount=1;ai.pSetLayouts=&set_layout;VKOK(vkAllocateDescriptorSets(device,&ai,&set));
   VkDescriptorBufferInfo info{target.owner->buffer,target.offset,128};
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.descriptorCount=1;
   w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&info;vkUpdateDescriptorSets(device,1,&w,0,nullptr);
   Barrier(VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT);
   vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
   vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);vkCmdDispatch(command,32,1,1);
  } else pending.push_back([target]{for(uint32_t i=0;i<32;++i){uint32_t v=0x71100000u+i;std::memcpy(target.owner->map+target.offset+i*4,&v,4);}});
 }
 void Submit() {
  if(gpu) {
   Barrier(VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);VKOK(vkEndCommandBuffer(command));
   VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&command;
   VKOK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));VKOK(vkQueueWaitIdle(queue));
  } else for(auto& f:pending)f();
  for(const auto& a:allocations)CHECK(std::memcmp(a->map,a->expected.data(),a->expected.size())==0);
 }
 ~Device() {
  if(!gpu)return;vkDeviceWaitIdle(device);
  vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);
  vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorPool(device,descriptors,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
  vkDestroyCommandPool(device,commands,nullptr);
  for(const auto& a:allocations){vkUnmapMemory(device,a->memory);vkDestroyBuffer(device,a->buffer,nullptr);vkFreeMemory(device,a->memory,nullptr);}
  vkDestroyDevice(device,nullptr);
  auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(instance,messenger,nullptr);
  vkDestroyInstance(instance,nullptr);
 }
};
namespace vk {
 enum class PipelineBindPoint{eGraphics,eCompute};
 enum class ShaderStageFlagBits{eMeshEXT,eVertex,eFragment,eCompute};
 enum class PipelineStageFlagBits{eComputeShader};
}
namespace DescriptorCache {enum class Stage{Mesh,Vertex,Pixel,Compute};}
namespace ShaderRecompiler::IR {
 struct MeshPointGroupPayload{uint32_t vertex_count,first_vertex,instance,root_lo,root_hi;};
 enum class ResourceKind{StorageImage,StorageImageUint};
}
struct Resource{bool written=false;ShaderRecompiler::IR::ResourceKind kind{};};
struct Program{struct{std::vector<Resource> images;}info;};
struct Runtime{};struct InputInfo{Runtime stage;};struct ShaderBufferDataOverride{};
struct Scenario;static Scenario* active=nullptr;
struct BufferCache{uint64_t epoch=1;uint64_t GetBindingEpoch()const{return epoch;}};
struct Handle{void bindPipeline(vk::PipelineBindPoint,int){}void dispatch(uint32_t,uint32_t,uint32_t);};
struct CommandBuffer{uint64_t generation=1;uint64_t GetRecordingGeneration()const{return generation;}::Handle Handle(){return {};}};
struct Pipeline{int pipeline=1,pipeline_layout=1;};
struct PipelineCache{
 Pipeline pipeline;
 template<class... T>Pipeline* CreateComputePipeline(T&&...);
 template<class... T>Pipeline* CreateGraphicsPipeline(T&&...);
};
struct RenderContext{
 BufferCache cache;PipelineCache pipelines;
 BufferCache* GetBufferCache(){return &cache;}PipelineCache* GetPipelineCache(){return &pipelines;}
};
static RenderContext* g_render_ctx=nullptr;
struct Scenario{
 Device& device;RenderContext context;CommandBuffer command;Binding current,bound,vertex,index;
 uint32_t pass=0,join_passes=1;bool graphics=false,mesh=false,pixel_join=false,index_join=false,reset_generation=false,reset_done=false;
 explicit Scenario(Device& d):device(d){current={d.Make(4096,0xa5),256};std::memset(current.owner->map+256,0,128);std::memset(current.owner->expected.data()+256,0,128);}
 void Join(){Binding next{device.Make(current.owner->payload*2,0xcd),current.offset+1024};device.Copy(current,next);current=next;++context.cache.epoch;}
 void Maybe(const char* kind){
  if(reset_generation && !reset_done && !std::strcmp(kind,graphics?"pixel":"compute")){++command.generation;reset_done=true;}
  if(pass>join_passes)return;
  if((!graphics && !std::strcmp(kind,"compute")) || (pixel_join && !std::strcmp(kind,"pixel")) ||
     (index_join && !std::strcmp(kind,"index")))Join();
 }
};
template<class... T>Pipeline* PipelineCache::CreateComputePipeline(T&&...){++active->pass;return &pipeline;}
template<class... T>Pipeline* PipelineCache::CreateGraphicsPipeline(T&&...){++active->pass;return &pipeline;}
void Handle::dispatch(uint32_t,uint32_t,uint32_t){active->device.Write(active->bound);}
static void ClearBounceCopies(){}
template<class... T>static void FlushBounceCopies(T&&...){}
static bool HasShaderBufferWrites(const Runtime&){return true;}
static void ShaderWriteBarrier(Handle,vk::PipelineStageFlagBits){}
template<class... T>static void LogDrawPhase(T&&...){}
template<class... T>static void SetDrawDebugPhase(T&&...){}
template<class... T>static int BuildGraphicsDynamicParams(T&&...){return 0;}
template<class... T>static void SetDynamicParams(T&&...){}
template<class... T>static void BindDrawVertexBuffers(T&&...){active->vertex=active->current;}
template<class... T>static void BindDrawIndexBuffer(T&&...){active->index=active->current;active->Maybe("index");}
static void BindDescriptors(uint64_t,CommandBuffer*,vk::PipelineBindPoint,int,const Runtime&,
 vk::ShaderStageFlagBits,DescriptorCache::Stage stage,const ShaderRecompiler::IR::MeshPointGroupPayload* =nullptr,
 const ShaderBufferDataOverride* =nullptr){
 if(stage==DescriptorCache::Stage::Pixel){active->Maybe("pixel");return;}
 active->bound=active->current;active->Maybe(stage==DescriptorCache::Stage::Compute?"compute":"vertex");
}
struct ShaderContext{int cs=0;int& GetCs(){return cs;}};
struct Draw{const char*name="fixture";uint32_t index_count=3,instance_count=1,first_instance=0;};
struct DrawState{
 bool merged_mesh=false,ps_active=true;int framebuffer=0,color_info[8]{},color_count=1,depth_info=0,vs_shader=0,ps_shader=0;
 InputInfo vs_input_info,ps_input_info;struct{uint32_t vertices_per_group=20;}mesh_assembly;uint64_t gs_root=0;Handle vk_buffer;
};
#include "buffer_binding_production_loops.inc"
static void Case(bool gpu,const char* shader,bool fixed,bool graphics,uint32_t joins,bool pixel,bool index,bool mesh=false,bool reset=false){
 Device device(gpu,shader);Scenario scenario(device);active=&scenario;g_render_ctx=&scenario.context;
 scenario.graphics=graphics;scenario.mesh=mesh;scenario.join_passes=joins;scenario.pixel_join=pixel;
 scenario.index_join=index;scenario.reset_generation=reset;
 if(graphics){if(fixed)RunDrawFixed();else RunDrawOld();}else{if(fixed)RunComputeFixed();else RunComputeOld();}
 device.Submit();bool live=true;
 for(uint32_t i=0;i<32;++i){uint32_t v;std::memcpy(&v,scenario.current.owner->map+scenario.current.offset+i*4,4);live=live && v==0x71100000u+i;}
 CHECK(live==(fixed || joins==0));
 if(fixed){CHECK(scenario.pass==joins+1 || (joins==0 && reset && scenario.pass==2));
  CHECK(scenario.bound.owner==scenario.current.owner && scenario.bound.offset==scenario.current.offset);
  if(graphics)CHECK(scenario.vertex.owner==scenario.current.owner && scenario.index.owner==scenario.current.owner);
 }
 std::printf("[BUFFER-REBIND] backend=%s path=%s fixed=%u joins_per_stage=%u rounds=%u live_owner_has_write=%u all_buffers_guards_exact=1 game_frame=0\n",
  gpu?"Vulkan":"CPU-ordered",graphics?(mesh?"Mesh-prepare":"graphics-prepare"):"compute",unsigned(fixed),joins,scenario.pass,unsigned(live));
}
int main(int argc,char**argv){
 try{
  CHECK(argc==3);const bool gpu=std::string(argv[1])=="--gpu";CHECK(gpu || std::string(argv[1])=="--cpu");
  for(bool graphics:{false,true}){
   Case(gpu,argv[2],false,graphics,1,graphics,false);
   Case(gpu,argv[2],true,graphics,1,graphics,false);
   Case(gpu,argv[2],true,graphics,2,graphics,false);
  }
  Case(gpu,argv[2],false,true,1,false,true);Case(gpu,argv[2],true,true,1,false,true);
  Case(gpu,argv[2],true,true,1,true,true);Case(gpu,argv[2],true,true,1,true,true,true);
  Case(gpu,argv[2],true,false,0,false,false,false,true);Case(gpu,argv[2],true,true,0,false,false,false,true);
  CHECK(validation_issues==0);
  std::printf("PASS production preparation loops: old lost-write controls, repeated/cross-stage joins, late index/vertex refresh, Mesh preparation, command reset; validation_issues=%u\n",validation_issues.load());
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
