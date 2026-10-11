static void Require(bool v){if(!v)throw std::runtime_error("refresh regression");}
int main(){
 for(int mode=0;mode<10;mode++){
  TextureCache cache;VideoOutVulkanImage image;
  auto record=std::make_unique<TextureCache::CachedImage>();record->image=&image;
  auto* owner=record.get();cache.m_images.push_back(std::move(record));
  refresh_uploads=0;refresh_buffer=&cache.m_buffer_cache;
  if(mode==1)owner->gpu_modified=true;
  if(mode==2)cache.m_memory_tracker.dirty=true;
  if(mode==3 || mode==4 || mode==5 || mode==7 || mode==8)cache.m_buffer_cache.gpu=true;
  if(mode==4)owner->buffer_modified=true;
  if(mode==5)owner->video_out.compression=VideoOutCompression::Dcc256_256_0;
  if(mode==6){owner->gpu_modified=true;cache.m_memory_tracker.dirty=true;}
  if(mode==7)owner->gpu_modified=true;
  if(mode==8){owner->buffer_modified=true;cache.m_buffer_cache.coherent=false;}
  if(mode==9)cache.m_buffer_cache.cpu=true;
  bool rejected=false;try{cache.RefreshVideoOut(&image,true);}catch(const std::runtime_error&){rejected=true;}
  const bool reject=mode==5 || mode==6 || mode==7 || mode==8;
  const bool upload=mode==2 || mode==3 || mode==4 || mode==9;
  Require(rejected==reject);Require(refresh_uploads==(upload?1u:0u));
  Require(cache.m_buffer_cache.obtains==((mode==3||mode==4||mode==8||mode==9)?1u:0u));
  if(upload){Require(!owner->buffer_modified && !cache.m_memory_tracker.dirty);cache.RefreshVideoOut(&image,true);Require(refresh_uploads==1);}
  if(mode==8)Require(owner->buffer_modified);
 }
 std::puts("PASS: VideoOut native owner preservation and ordered buffer refresh");
}
