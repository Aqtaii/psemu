#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#define EXIT(...) throw std::runtime_error("ownership rejected")
struct VideoOutVulkanImage {};
struct VideoOutInfo {uint64_t address=0x1992f9a0000,size=0x1fe0000;VideoOutCompression compression=VideoOutCompression::Uncompressed;};
struct RefreshTracker {
 bool dirty=false;
 bool IsRegionCpuModified(uint64_t,uint64_t){return dirty;}
 template<class R,class U> void ForEachUploadRange(uint64_t,uint64_t,bool,R,U upload){dirty=false;upload();}
};
struct RefreshSource {bool cpu_current=true;void* buffer=nullptr;uint64_t address=0,size=0;};
inline bool IsCoherentGuestImageSource(const RefreshSource& s,uint64_t a,uint64_t n){return s.cpu_current&&s.address==a&&s.size==n;}
struct RefreshBufferCache {
 bool cpu=false,gpu=false,coherent=true;
 unsigned obtains=0;
 bool IsCachedRegionCpuModified(uint64_t,uint64_t){return cpu;}
 bool IsRegionGpuModified(uint64_t,uint64_t){return gpu;}
 RefreshSource ObtainBufferForImage(uint64_t a,uint64_t n){obtains++;cpu=false;gpu=false;return {coherent,nullptr,a,n};}
};
inline unsigned refresh_uploads=0;
inline RefreshBufferCache* refresh_buffer=nullptr;
namespace ImageOps {inline void UploadVideoOut(void*,VideoOutVulkanImage*,const VideoOutInfo&,bool){
 if(refresh_buffer->gpu || refresh_buffer->cpu)std::terminate();
 refresh_uploads++;
}}
struct TextureCache {
 struct CachedImage {
  enum class Kind {VideoOut,Other};
  VideoOutVulkanImage* image=nullptr;Kind kind=Kind::VideoOut;
  bool gpu_modified=false,buffer_modified=false;void* ctx=nullptr;VideoOutInfo video_out;
 };
 std::mutex m_resource_mutex;
 std::vector<std::unique_ptr<CachedImage>> m_images;
 RefreshTracker m_memory_tracker;
 RefreshBufferCache m_buffer_cache;
 void RefreshVideoOut(VideoOutVulkanImage*,bool);
};
