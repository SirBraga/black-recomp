#include "runtime/gs/gs_parallel_api.h"
#include "gs_interface.hpp"
#include "context.hpp"
#include "thread_id.hpp"
#include <dlfcn.h>
#include <memory>
#include <cstring>
#include <cstdio>
#include <vector>
#include <deque>
#include <algorithm>
#include <string>
#include <vulkan/vulkan_metal.h>

#if defined(__APPLE__)
extern "C" void* black_metal_presenter_create(void*,VkDevice,VkQueue,PFN_vkExportMetalObjectsEXT);
extern "C" int black_metal_presenter_present(void*,VkDevice,VkImage,VkFormat,uint32_t,uint32_t,PFN_vkExportMetalObjectsEXT);
extern "C" void black_metal_presenter_destroy(void*);
#endif
using R=ParallelGS::RegisterAddr;
struct MetalExportInstanceFactory final : Vulkan::InstanceFactory {
 PFN_vkCreateInstance createInstance=nullptr;
 explicit MetalExportInstanceFactory(PFN_vkCreateInstance fn):createInstance(fn){}
 VkInstance create_instance(const VkInstanceCreateInfo* source) override {
  if(!createInstance||!source)return VK_NULL_HANDLE;
  VkExportMetalObjectCreateInfoEXT queueExport{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
  queueExport.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT;
  queueExport.pNext=source->pNext;
  VkExportMetalObjectCreateInfoEXT deviceExport{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
  deviceExport.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT;
  deviceExport.pNext=&queueExport;
  VkInstanceCreateInfo info=*source;info.pNext=&deviceExport;
  VkInstance instance=VK_NULL_HANDLE;
  return createInstance(&info,nullptr,&instance)==VK_SUCCESS?instance:VK_NULL_HANDLE;
 }
};
struct Host {
 std::unique_ptr<MetalExportInstanceFactory> metalExportFactory;
 Vulkan::Context context;
 Vulkan::Device device;
 ParallelGS::GSInterface gs;
 Vulkan::BufferHandle scanoutBuffer;
 void* metalPresenter=nullptr;
 PFN_vkExportMetalObjectsEXT exportMetalObjects=nullptr;
 uint32_t scanoutBufPixels=0;
 uint64_t readbacks=0, primitives=0;
 struct Event {uint32_t kind,path;std::vector<uint8_t> bytes;};
 std::deque<Event> recent;
 bool badTransferDumped=false;
 FILE* capture=nullptr;
 uint64_t capturedBytes=0;
 uint64_t capturedEvents=0;
 bool currentEventCaptured=false;
 bool captureFull=false;
 bool snapshotTaken=false;
 bool automaticSnapshotArmed=false;
 ~Host(){if(capture)std::fclose(capture);}

 void snapshot_if_requested(const char* stage="scanout", int triggerContext=-1, uint32_t packetPrefix=0) {
  const char* request=std::getenv("PS2X_PARALLEL_SNAPSHOT_REQUEST");
  if(!request||snapshotTaken)return;
  FILE* trigger=std::fopen(request,"rb");if(!trigger)return;std::fclose(trigger);
  const uint32_t clutInstance=gs.debug_clut_instance();
  gs.flush();
  const void* vram=gs.map_vram_read(0,4*1024*1024);
  const std::string prefix(request);
  Vulkan::BufferCreateInfo clutInfo{};
  clutInfo.size=ParallelGS::CLUTSize;clutInfo.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;clutInfo.domain=Vulkan::BufferDomain::CachedHost;
  auto clutCopy=device.create_buffer(clutInfo);
  auto copy=device.request_command_buffer();
  copy->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
  copy->copy_buffer(*clutCopy,0,*gs.debug_clut_buffer(),size_t(clutInstance)*ParallelGS::CLUTSize,ParallelGS::CLUTSize);
  copy->barrier(VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
  Vulkan::Fence copied;device.submit(copy,&copied);copied->wait();
  const void* cachedClut=device.map_host_buffer(*clutCopy,Vulkan::MEMORY_ACCESS_READ_BIT);
  if(!cachedClut)return;
  FILE* clutFile=std::fopen((prefix+".clut.bin").c_str(),"wb");
  bool clutComplete=clutFile&&std::fwrite(cachedClut,1,ParallelGS::CLUTSize,clutFile)==ParallelGS::CLUTSize;
  if(clutFile)clutComplete=std::fclose(clutFile)==0&&clutComplete;
  device.unmap_host_buffer(*clutCopy,Vulkan::MEMORY_ACCESS_READ_BIT);
  if(!clutComplete)return;
  FILE* memory=std::fopen((prefix+".vram.bin").c_str(),"wb");
  if(!memory)return;
  bool complete=std::fwrite(vram,1,4*1024*1024,memory)==4*1024*1024;
  complete=std::fclose(memory)==0&&complete;
  FILE* registers=std::fopen((prefix+".registers.json").c_str(),"wb");
  if(!registers)return;
  const auto& state=gs.get_register_state();
  std::fprintf(registers,"{\"stage\":\"%s\",\"trigger_context\":%d,\"capture_current_event\":%s,\"capture_events\":%llu,\"capture_bytes\":%llu,\"trigger_packet_bytes\":%u,\"clut_instance\":%u,\"readbacks\":%llu,\"texclut\":\"%016llx\",\"texa\":\"%016llx\",\"contexts\":[",
               stage,triggerContext,currentEventCaptured?"true":"false",(unsigned long long)capturedEvents,(unsigned long long)capturedBytes,packetPrefix,clutInstance,(unsigned long long)readbacks,(unsigned long long)state.texclut.bits,(unsigned long long)state.texa.bits);
  for(unsigned i=0;i<2;i++){
   const auto& c=state.ctx[i];
   std::fprintf(registers,"%s{\"tex0\":\"%016llx\",\"tex1\":\"%016llx\",\"frame\":\"%016llx\",\"clamp\":\"%016llx\"}",
                i?",":"",(unsigned long long)c.tex0.bits,(unsigned long long)c.tex1.bits,(unsigned long long)c.frame.bits,(unsigned long long)c.clamp.bits);
  }
  std::fprintf(registers,"]}\n");complete=std::fclose(registers)==0&&complete;
  if(complete){snapshotTaken=true;if(capture){std::fclose(capture);capture=nullptr;captureFull=true;}std::remove(request);std::fprintf(stderr,"[parallel-gs] captured GPU VRAM and texture registers at readback=%llu to %s\n",(unsigned long long)readbacks,request);}
 }

 void remember(uint32_t kind,uint32_t path,const uint8_t* data,uint32_t size) {
  currentEventCaptured=false;
  if(!automaticSnapshotArmed&&!snapshotTaken){
   const char* after=std::getenv("PS2X_PARALLEL_SNAPSHOT_AFTER_READBACKS");
   const char* request=std::getenv("PS2X_PARALLEL_SNAPSHOT_REQUEST");
   if(after&&request&&readbacks>=std::strtoull(after,nullptr,0)){
    if(FILE* trigger=std::fopen(request,"wb")){std::fclose(trigger);automaticSnapshotArmed=true;}
   }
  }
  if(const char* filename=std::getenv("PS2X_PARALLEL_STREAM_CAPTURE")){
   const char* start=std::getenv("PS2X_PARALLEL_CAPTURE_AFTER_READBACKS");
   const uint64_t delay=start?std::strtoull(start,nullptr,0):0;
   if(readbacks>=delay && !captureFull){
   if(!capture && !capturedBytes)capture=std::fopen(filename,"wb");
   const char* limitEnv=std::getenv("PS2X_PARALLEL_CAPTURE_MAX_MB");
   const uint64_t limit=uint64_t(limitEnv?std::clamp(std::strtoul(limitEnv,nullptr,0),16ul,1024ul):128ul)*1024*1024;
   if(capture && capturedBytes+12+size<=limit){
    uint32_t header[3]={kind,path,size};
    std::fwrite(header,sizeof(header),1,capture);std::fwrite(data,size,1,capture);
    std::fflush(capture);capturedBytes+=12+size;++capturedEvents;currentEventCaptured=true;
   }else if(capture){captureFull=true;std::fprintf(stderr,"[parallel-gs] stream capture full; stopping at a contiguous event boundary\n");}
   }
  }
  if(!std::getenv("PS2X_PARALLEL_BAD_TRANSFER_CAPTURE"))return;
  if(size==0||size>1024*1024)return;
  recent.push_back({kind,path,std::vector<uint8_t>(data,data+size)});
  if(recent.size()>32)recent.pop_front();
 }
 void audit(const ParallelGS::RegisterState* observed=nullptr) {
  const char* filename=std::getenv("PS2X_PARALLEL_BAD_TRANSFER_CAPTURE");
  if(!filename||badTransferDumped)return;
  const auto& r=observed?*observed:gs.get_register_state();
  if(r.trxdir.desc.XDIR!=1)return;
  switch(r.bitbltbuf.desc.SPSM){case 0:case 1:case 2:case 10:case 19:case 20:case 27:case 36:case 44:case 48:case 49:case 50:case 58:return;default:break;}
  badTransferDumped=true;
  std::fprintf(stderr,"[parallel-gs-audit] invalid FIFO SPSM=%u bitblt=%016llx trxpos=%016llx trxreg=%016llx; saving %zu events to %s\n",r.bitbltbuf.desc.SPSM,(unsigned long long)r.bitbltbuf.bits,(unsigned long long)r.trxpos.bits,(unsigned long long)r.trxreg.bits,recent.size(),filename);
  if(FILE* f=std::fopen(filename,"wb")){for(const auto& e:recent){uint32_t header[3]={e.kind,e.path,uint32_t(e.bytes.size())};std::fwrite(header,sizeof(header),1,f);std::fwrite(e.bytes.data(),e.bytes.size(),1,f);}std::fclose(f);}
 }

};
static void* create(const char* path,const uint8_t* initial) {
 try {
  void* lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
  if(!lib){std::fprintf(stderr,"[parallel-gs] %s\n",dlerror());return nullptr;}
  auto loader=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vkGetInstanceProcAddr"));
  if(!Vulkan::Context::init_loader(loader))return nullptr;
  auto h=std::make_unique<Host>();
#if defined(__APPLE__)
  const bool nativeRequested=std::getenv("PS2X_GS_NATIVE_PRESENT")!=nullptr;
  const char* metalObjects=VK_EXT_METAL_OBJECTS_EXTENSION_NAME;
  if(nativeRequested){auto createInstance=reinterpret_cast<PFN_vkCreateInstance>(loader(nullptr,"vkCreateInstance"));h->metalExportFactory=std::make_unique<MetalExportInstanceFactory>(createInstance);h->context.set_instance_factory(h->metalExportFactory.get());}
  if(!h->context.init_instance_and_device(nullptr,0,nativeRequested?&metalObjects:nullptr,nativeRequested?1u:0u))return nullptr;
#else
  if(!h->context.init_instance_and_device(nullptr,0,nullptr,0))return nullptr;
#endif
  h->device.set_context(h->context);
#if defined(__APPLE__)
  if(nativeRequested){auto getProc=Vulkan::Context::get_instance_proc_addr();auto getDeviceProc=reinterpret_cast<PFN_vkGetDeviceProcAddr>(getProc(h->device.get_instance(),"vkGetDeviceProcAddr"));if(getDeviceProc)h->exportMetalObjects=reinterpret_cast<PFN_vkExportMetalObjectsEXT>(getDeviceProc(h->device.get_device(),"vkExportMetalObjectsEXT"));}
#endif
  if(!h->gs.init(&h->device,{}))return nullptr;
  ParallelGS::Hacks hacks{};hacks.allow_blend_demote=true;h->gs.set_hacks(hacks);
  std::memcpy(h->gs.map_vram_write(0,4*1024*1024),initial,4*1024*1024);
  h->gs.end_vram_write(0,4*1024*1024);
  h->remember(5,0,initial,4*1024*1024);
  std::fprintf(stderr,"[parallel-gs] GPU GS ready; no CPU raster fallback\n");
  return h.release();
 } catch(const std::exception& e){std::fprintf(stderr,"[parallel-gs] %s\n",e.what());return nullptr;}
}
// Calls are serialized by GSParallelBackend; reuse Granite command-pool slot 0.
static Host& host(void* p){Util::register_thread_index(0);return *static_cast<Host*>(p);}
static void destroy(void* p){auto& h=host(p);h.device.wait_idle();
#if defined(__APPLE__)
 if(h.metalPresenter)black_metal_presenter_destroy(h.metalPresenter);
#endif
 delete static_cast<Host*>(p);}
static int attachWindow(void* p,void* window){
#if defined(__APPLE__)
 auto& h=host(p);if(!h.exportMetalObjects||!window)return 0;
 h.metalPresenter=black_metal_presenter_create(window,h.device.get_device(),h.device.get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS],h.exportMetalObjects);
 return h.metalPresenter?1:0;
#else
 (void)p;(void)window;return 0;
#endif
}
static void reset(void* p){host(p).remember(6,0,nullptr,0);host(p).gs.flush();host(p).gs.reset_context_state();}
static void gif(void* p,uint32_t path,const uint8_t* data,uint32_t size){
 auto& h=host(p);h.remember(1,path,data,size);
 // Inspect A+D transport without fragmenting the renderer's optimized packet handler.
 if(std::getenv("PS2X_PARALLEL_BAD_TRANSFER_CAPTURE")&&!h.badTransferDumped){
  auto state=h.gs.get_register_state();auto parser=h.gs.get_gif_path(path);
  for(uint32_t offset=0;offset+16<=size;offset+=16){
   if(parser.loop>=parser.tag.NLOOP){std::memcpy(&parser.tag,data+offset,16);parser.loop=0;parser.reg=0;continue;}
   uint32_t nreg=parser.tag.NREG?parser.tag.NREG:16;
   if(parser.tag.FLG==ParallelGS::GIFTagBits::PACKED){
    unsigned descriptor=unsigned(parser.tag.REGS>>(4*parser.reg))&15;
    if(descriptor==14){uint64_t words[2];std::memcpy(words,data+offset,16);
     switch(words[1]&255){case 0x50:state.bitbltbuf.bits=words[0];break;case 0x51:state.trxpos.bits=words[0];break;case 0x52:state.trxreg.bits=words[0];break;case 0x53:state.trxdir.bits=words[0];h.audit(&state);break;default:break;}
    }
    if(++parser.reg==nreg){parser.reg=0;parser.loop++;}
   }else if(parser.tag.FLG==ParallelGS::GIFTagBits::REGLIST){
    for(unsigned half=0;half<2&&parser.loop<parser.tag.NLOOP;half++)if(++parser.reg==nreg){parser.reg=0;parser.loop++;}
   }else parser.loop++;
  }
 }
 // Optional one-shot capture at the first changed indexed TEX0 with CLD.
 // Only split packets while an explicit trigger exists; normal rendering stays batched.
 if(std::getenv("PS2X_PARALLEL_SNAPSHOT_ON_TEX0")&&!h.snapshotTaken){
  const char* request=std::getenv("PS2X_PARALLEL_SNAPSHOT_REQUEST");
  FILE* trigger=request?std::fopen(request,"rb"):nullptr;
  if(trigger){
   std::fclose(trigger);
   uint32_t offset=0;
   for(;offset+16<=size&&!h.snapshotTaken;offset+=16){
    const auto& before=h.gs.get_register_state();
    const uint64_t oldTex[2]={before.ctx[0].tex0.bits,before.ctx[1].tex0.bits};
    h.gs.gif_transfer(path,data+offset,16);
    const auto& after=h.gs.get_register_state();
    for(unsigned c=0;c<2;c++){
     const uint64_t tex=after.ctx[c].tex0.bits;
     const unsigned psm=(tex>>20)&63;
     const char* filter=std::getenv("PS2X_PARALLEL_SNAPSHOT_PSM");
     const char* exactTex0=std::getenv("PS2X_PARALLEL_SNAPSHOT_TEX0");
     if((!filter||psm==std::strtoul(filter,nullptr,0))&&
        (!exactTex0||tex==std::strtoull(exactTex0,nullptr,0))&&
        tex!=oldTex[c]&&((tex>>61)&7)!=0&&(psm==19||psm==20||psm==27||psm==36||psm==44)){
      h.snapshot_if_requested("tex0",int(c),offset+16);break;
     }
    }
   }
   if(offset<size)h.gs.gif_transfer(path,data+offset,size-offset);
   h.audit();return;
  }
 }
 h.gs.gif_transfer(path,data,size);h.audit();
}
static void reg(void* p,uint8_t address,uint64_t value){if(address<=0x62){auto& h=host(p);h.remember(2,address,reinterpret_cast<const uint8_t*>(&value),8);h.gs.write_register(static_cast<R>(address),value);h.audit();}}
static void image(void* p,const uint8_t* data,uint32_t size){
 host(p).remember(3,0,data,size);
 // HWREG supplies 64 bits; the final word is zero-padded exactly as DMA transport.
 for(uint32_t offset=0;offset<size;offset+=8){uint64_t word=0;std::memcpy(&word,data+offset,std::min(8u,size-offset));host(p).gs.write_register(R::HWREG,word);}
}
static void flush(void* p){host(p).gs.flush();}
static void wait(void* p){host(p).gs.flush();host(p).device.wait_idle();}
static void read(void* p,uint32_t offset,uint8_t* out,uint32_t size){
 auto& h=host(p);std::memcpy(out,h.gs.map_vram_read(offset,size),size);
 auto stats=h.gs.consume_flush_stats();h.primitives+=stats.num_primitives;
 if(++h.readbacks==1||h.readbacks%120==0)std::fprintf(stderr,"[parallel-gs] primitives=%llu readbacks=%llu\n",(unsigned long long)h.primitives,(unsigned long long)h.readbacks);
}
static void write(void* p,uint32_t offset,const uint8_t* in,uint32_t size){host(p).remember(5,offset,in,size);std::memcpy(host(p).gs.map_vram_write(offset,size),in,size);host(p).gs.end_vram_write(offset,size);}
static void fifo(void* p,uint8_t* out,uint32_t size){host(p).gs.read_transfer_fifo(out,size/16);}
static void clear(void* p,uint32_t fbp,uint32_t bw,uint32_t x0,uint32_t y0,uint32_t x1,uint32_t y1,uint32_t rgba,uint32_t fbmsk,uint32_t fba){
 uint32_t args[]={fbp,bw,x0,y0,x1,y1,rgba,fbmsk,fba};
 host(p).remember(4,0,reinterpret_cast<const uint8_t*>(args),sizeof(args));
 auto& g=host(p).gs;g.flush();auto saved=g.get_register_state();
 g.write_register(R::PRMODECONT,uint64_t(1));g.write_register(R::PRIM,uint64_t(6));
 g.write_register(R::FRAME_1,uint64_t(fbp)|(uint64_t(bw)<<16)|(uint64_t(fbmsk)<<32));
 g.write_register(R::ZBUF_1,uint64_t(1)<<32);g.write_register(R::XYOFFSET_1,uint64_t(0));
 g.write_register(R::SCISSOR_1,uint64_t(x0)|(uint64_t(x1)<<16)|(uint64_t(y0)<<32)|(uint64_t(y1)<<48));
 g.write_register(R::TEST_1,uint64_t(0));g.write_register(R::SCANMSK,uint64_t(0));g.write_register(R::FBA_1,uint64_t(fba&1));
 g.write_register(R::RGBAQ,uint64_t(rgba)|(uint64_t(0x3f800000)<<32));
 g.write_register(R::XYZ2,uint64_t(x0*16)|(uint64_t(y0*16)<<16));
 g.write_register(R::XYZ2,uint64_t((x1+1)*16)|(uint64_t((y1+1)*16)<<16));
 g.flush();g.get_register_state()=saved;g.clobber_register_state();
}
static void setPriv64(void* dst,uint64_t value){std::memcpy(dst,&value,8);}
static int scanout(void* p,GSParallelScanout* req){
 if(!req)return 0;
 auto& h=host(p);
 auto& priv=h.gs.get_priv_register_state();
 setPriv64(&priv.pmode,req->pmode);setPriv64(&priv.smode1,req->smode1);setPriv64(&priv.smode2,req->smode2);
 setPriv64(&priv.dispfb1,req->dispfb1);setPriv64(&priv.display1,req->display1);
 setPriv64(&priv.dispfb2,req->dispfb2);setPriv64(&priv.display2,req->display2);
 setPriv64(&priv.bgcolor,req->bgcolor);
 h.gs.flush();
 ParallelGS::VSyncInfo info{};
 info.phase=uint32_t(req->vsyncTick&1ull);
 info.dst_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
 info.dst_stage=VK_PIPELINE_STAGE_2_COPY_BIT;
 info.dst_access=VK_ACCESS_2_TRANSFER_READ_BIT;
 info.force_progressive=true;info.anti_blur=true;
 info.adapt_to_internal_horizontal_resolution=true;info.raw_circuit_scanout=true;
 info.internal_resolution_scanout=true;
 auto result=h.gs.vsync(info);
 auto stats=h.gs.consume_flush_stats();h.primitives+=stats.num_primitives;
 if(++h.readbacks==1||h.readbacks%120==0)
  std::fprintf(stderr,"[parallel-gs] primitives=%llu readbacks=%llu scanout=%ux%u smode1=%016llx\n",
               (unsigned long long)h.primitives,(unsigned long long)h.readbacks,
               result.image?result.image->get_width():0u,result.image?result.image->get_height():0u,(unsigned long long)req->smode1);
 if(!result.image)return 0;
 const uint32_t iw=result.image->get_width(),ih=result.image->get_height();
#if defined(__APPLE__)
 if(h.metalPresenter&&h.exportMetalObjects){
  const int presented=black_metal_presenter_present(h.metalPresenter,h.device.get_device(),result.image->get_image(),result.image->get_format(),iw,ih,h.exportMetalObjects);
  if(presented){req->width=iw;req->height=ih;}
  return presented;
 }
#endif
 if(!req->rgba||!req->stride||!req->maxHeight)return 0;
 const uint32_t w=std::min(iw,req->stride/4u),ht=std::min(ih,req->maxHeight);
 if(!w||!ht)return 0;
 const uint32_t pixels=iw*ih;
 if(!h.scanoutBuffer||h.scanoutBufPixels<pixels){
  Vulkan::BufferCreateInfo bufinfo{};
  bufinfo.size=size_t(pixels)*4u;bufinfo.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bufinfo.domain=Vulkan::BufferDomain::CachedHost;
  h.scanoutBuffer=h.device.create_buffer(bufinfo);h.scanoutBufPixels=pixels;
 }
 {
  auto cmd=h.device.request_command_buffer();
  cmd->copy_image_to_buffer(*h.scanoutBuffer,*result.image,0,{},{iw,ih,1},0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1});
  cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
  // Wait for the copy that makes these pixels host-visible, not for all
  // outstanding work on the device. Keep the buffer alive until completion.
  Vulkan::Fence copied;
  h.device.submit(cmd,&copied);
  copied->wait();
 }
 auto* src=static_cast<uint8_t*>(h.device.map_host_buffer(*h.scanoutBuffer,Vulkan::MEMORY_ACCESS_READ_BIT));
 if(!src)return 0;
 for(uint32_t y=0;y<ht;++y){
  uint8_t* dst=req->rgba+size_t(y)*req->stride;
  std::memcpy(dst,src+size_t(y)*iw*4u,size_t(w)*4u);
  for(uint32_t x=0;x<w;++x)dst[x*4u+3u]=255u;
 }
 req->width=w;req->height=ht;
 if(!std::getenv("PS2X_PARALLEL_SNAPSHOT_ON_TEX0"))h.snapshot_if_requested();
 return 1;
}
extern "C" const GSParallelAPI* black_parallel_gs_api(){static const GSParallelAPI api{5,create,destroy,reset,gif,reg,image,flush,wait,read,write,fifo,clear,scanout,attachWindow};return &api;}
