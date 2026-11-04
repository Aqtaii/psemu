#pragma once
#include "graphics/host_gpu/diagnosticProbeControl.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/renderer/computeBufferProbe.h"
#include "graphics/host_gpu/renderer/descriptors.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/host_gpu/renderer/sceneProbeWindow.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <fmt/format.h>

namespace Libs::Graphics {
inline bool SceneFrameProbe(uint64_t submit) {
 static const bool enabled=[] {const auto* x=std::getenv("PSEMU_ASTRO_SCENE_PROBE");return x && std::strcmp(x,"1")==0;}();
 if(!enabled || !AstroFrameProbeGateOpen())return false;
 static SceneProbeSubmitWindow window;
 return window.Include(submit);
}
// Observe the exact successfully bound view, including any alignment bounce.
// Copies run before drawing, after all preparation/owner-epoch checks.
inline void ProbeSceneTransform(CommandBuffer* command,const ShaderStageRuntime& stage,
 const std::vector<BufferView>& views,uint64_t submit,uint64_t root) {
 if(!SceneFrameProbe(submit) || stage.program->instruction_code_hash!=0x1cd649e2963a321eull || views.size()<=3)return;
 static std::atomic_uint count{0};if(count.fetch_add(1)>=16)return;
 const auto d=DecodeNativeDescriptor<ShaderBufferResource>(stage.resources->buffers[3]);
 const auto& view=views[3];if(!view.buffer)return;
 struct Copy {uint32_t record;uint64_t source,destination;};
 std::vector<Copy> copies;
 for(auto i:{12223u,12159u,12095u,12222u,12158u,12094u,12221u,12157u}) {
  uint64_t offset=0;
  if(AstroProducerProbeOffset(view.offset,view.range,view.buffer->buffer_size,d.Stride(),d.NumRecords(),i,&offset))
   copies.push_back({i,offset,copies.size()*128ull});
 }
 auto desc_line=fmt::format("[SCENE-MESH-DESC] submit={} root=0x{:x} slot=3 guest=0x{:x} stride={} records={} view_offset={} view_bytes={} selected_records={} raw={:08x} {:08x} {:08x} {:08x}\n",submit,root,d.Base48(),d.Stride(),d.NumRecords(),view.offset,view.range,copies.size(),d.fields[0],d.fields[1],d.fields[2],d.fields[3]);
 std::fwrite(desc_line.data(),1,desc_line.size(),stdout);
 if(copies.empty())return;
 auto* ctx=g_render_ctx->GetGraphicCtx();
 auto staging=std::shared_ptr<VulkanBuffer>(new VulkanBuffer,[ctx](auto* x){VulkanDeleteBuffer(ctx,x);delete x;});
 staging->usage=vk::BufferUsageFlagBits::eTransferDst;
 staging->memory.property=vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent;
 VulkanCreateBuffer(ctx,copies.size()*128,staging.get());
 std::vector<AstroProducerBufferCopy> commands;
 for(auto c:copies)commands.push_back({view.buffer->buffer,c.source,c.destination,128});
 RecordAstroProducerBufferCopies(command->Handle(),staging->buffer,copies.size()*128,commands,VULKAN_HPP_DEFAULT_DISPATCHER);
 const auto guest=d.Base48(),view_offset=view.offset,allocation=view.buffer->memory.unique_id;
 command->OnFenceComplete([ctx,staging,copies,guest,view_offset,allocation,submit,root] {
  void* mapped=nullptr;VulkanMapMemory(ctx,&staging->memory,&mapped);
  for(auto c:copies) {
   std::array<uint32_t,32> words{};std::memcpy(words.data(),static_cast<uint8_t*>(mapped)+c.destination,128);
   auto line=fmt::format("[SCENE-MESH-INPUT] submit={} root=0x{:x} slot=3 guest=0x{:x} allocation={} view_offset={} record={} source=completed-bound-view raw=",submit,root,guest,allocation,view_offset,c.record);
   for(auto w:words)line+=fmt::format(" {:08x}",w);line+='\n';std::fwrite(line.data(),1,line.size(),stdout);
  }
  VulkanUnmapMemory(ctx,&staging->memory);
 });
}
inline void ProbeSceneImage(CommandBuffer* command,VulkanImage* image,uint64_t submit,
 uint64_t shader,uint64_t guest,uint32_t slot,const char* phase,bool output) {
 if(!SceneFrameProbe(submit) || !image || !image->image || !image->extent.width || !image->extent.height ||
    image->layout==vk::ImageLayout::eUndefined || image->layout==vk::ImageLayout::ePreinitialized)return;
 static std::atomic_uint count{0};if(count.fetch_add(1)>=1024)return;
 uint32_t bytes=0,rgb_bytes=0;bool packed_alpha=false;bool floating=false;uint32_t bits=0,channels=0;bool depth=false;
 switch(image->format) {
 case vk::Format::eR8G8B8A8Unorm:case vk::Format::eR8G8B8A8Srgb:case vk::Format::eB8G8R8A8Unorm:case vk::Format::eB8G8R8A8Srgb:bytes=4;rgb_bytes=3;break;
 case vk::Format::eA2B10G10R10UnormPack32:case vk::Format::eA2R10G10B10UnormPack32:bytes=4;packed_alpha=true;break;
 case vk::Format::eB10G11R11UfloatPack32:bytes=4;break;
 case vk::Format::eR16G16B16A16Unorm:bytes=8;rgb_bytes=6;break;
 case vk::Format::eR8G8Unorm:bytes=2;break;
 case vk::Format::eR8Unorm:bytes=1;break;
 case vk::Format::eR16G16B16A16Sfloat:bytes=8;floating=true;bits=16;channels=4;break;
 case vk::Format::eR16G16Sfloat:bytes=4;floating=true;bits=16;channels=2;break;
 case vk::Format::eR16Sfloat:bytes=2;floating=true;bits=16;channels=1;break;
 case vk::Format::eR32Sfloat:bytes=4;floating=true;bits=32;channels=1;break;
 case vk::Format::eR32G32Sfloat:bytes=8;floating=true;bits=32;channels=2;break;
 case vk::Format::eR32G32B32A32Sfloat:bytes=16;floating=true;bits=32;channels=4;break;
 case vk::Format::eD32Sfloat:bytes=4;floating=true;bits=32;channels=1;depth=true;break;
 case vk::Format::eD16Unorm:bytes=2;depth=true;break;
 default:printf("[SCENE-IMAGE] submit=%llu phase=%s shader=0x%llx slot=%u guest=0x%llx format=%u unsupported=1\n",(unsigned long long)submit,phase,(unsigned long long)shader,slot,(unsigned long long)guest,unsigned(image->format));return;
 }
 static std::atomic_uint full_count{0};
 const bool full=output && full_count.fetch_add(1)<48;
 const auto w=image->extent.width,h=image->extent.height;
 const uint64_t pixels=full?uint64_t(w)*h:1024, stride=full?bytes:(bytes+3u)&~3u, size=pixels*stride;
 if(size>128ull*1024*1024)return;
 auto* ctx=g_render_ctx->GetGraphicCtx();
 auto staging=std::shared_ptr<VulkanBuffer>(new VulkanBuffer,[ctx](auto* x){VulkanDeleteBuffer(ctx,x);delete x;});
 staging->usage=vk::BufferUsageFlagBits::eTransferDst;
 staging->memory.property=vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent;
 VulkanCreateBuffer(ctx,size,staging.get());
 auto old=image->layout;vk::ImageMemoryBarrier barrier{};
 barrier.srcAccessMask=vk::AccessFlagBits::eMemoryWrite|vk::AccessFlagBits::eMemoryRead;
 barrier.dstAccessMask=vk::AccessFlagBits::eTransferRead;
 barrier.oldLayout=old;barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
 barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 barrier.image=image->image;barrier.subresourceRange={depth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,1,0,1};
 auto vk=command->Handle();
 vk.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},0,nullptr,0,nullptr,1,&barrier);
 std::vector<vk::BufferImageCopy> regions(full?1:1024);
 for(uint32_t i=0;i<regions.size();++i) {
  auto& r=regions[i];r.bufferOffset=uint64_t(i)*stride;
  r.imageSubresource={depth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,0,1};
  r.imageOffset=full?vk::Offset3D{0,0,0}:vk::Offset3D{int32_t((uint64_t(i%32)*w+w/2)/32),int32_t((uint64_t(i/32)*h+h/2)/32),0};
  r.imageExtent=full?vk::Extent3D{w,h,1}:vk::Extent3D{1,1,1};
 }
 vk.copyImageToBuffer(image->image,vk::ImageLayout::eTransferSrcOptimal,staging->buffer,uint32_t(regions.size()),regions.data());
 barrier.srcAccessMask=vk::AccessFlagBits::eTransferRead;barrier.dstAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite;
 barrier.oldLayout=vk::ImageLayout::eTransferSrcOptimal;barrier.newLayout=old;
 vk.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,{},0,nullptr,0,nullptr,1,&barrier);
 vk::BufferMemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eTransferWrite;host.dstAccessMask=vk::AccessFlagBits::eHostRead;
 host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=staging->buffer;host.size=size;
 vk.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},0,nullptr,1,&host,0,nullptr);
 auto id=image->memory.unique_id;auto format=uint32_t(image->format);
 command->OnFenceComplete([=]{
  void* mapped=nullptr;VulkanMapMemory(ctx,&staging->memory,&mapped);
  auto* data=static_cast<const uint8_t*>(mapped);uint64_t nonzero=0,nonfinite=0;float peak=0;
  for(uint64_t i=0;i<pixels;++i) {
   bool nz=false;
   if(!floating) {
    for(uint32_t b=0;b<(rgb_bytes?rgb_bytes:bytes);++b)nz|=data[i*stride+b]!=0;
    if(packed_alpha) {uint32_t word;std::memcpy(&word,data+i*stride,4);nz=(word&0x3fffffffu)!=0;}
   }
   if(floating)for(uint32_t c=0;c<channels;++c) {
    float v;
    if(bits==32)std::memcpy(&v,data+i*stride+c*4,4);
    else {uint16_t raw;std::memcpy(&raw,data+i*stride+c*2,2);const auto e=(raw>>10)&31,f=raw&1023;
     v=e==31?std::numeric_limits<float>::infinity():e==0?std::ldexp(float(f),-24):std::ldexp(1.f+float(f)/1024.f,int(e)-15);if(raw&0x8000)v=-v;}
    if(!std::isfinite(v))++nonfinite;else if(c<3) {peak=std::max(peak,std::abs(v));nz|=v!=0;}
   }
   nonzero+=nz;
  }
  auto line=fmt::format("[SCENE-IMAGE] submit={} phase={} shader=0x{:x} slot={} guest=0x{:x} allocation={} format={} extent={}x{} full={} rgb_nonzero={}/{} float_peak={} nonfinite={} source=completed-fence\n",submit,phase,shader,slot,guest,id,format,w,h,full,nonzero,pixels,peak,nonfinite);
  std::fwrite(line.data(),1,line.size(),stdout);VulkanUnmapMemory(ctx,&staging->memory);
 });
}
} // namespace Libs::Graphics
