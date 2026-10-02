#!/usr/bin/env python3
"""Run actual fixed CRC capture/allocation/restoration against bounded mocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
using io_connect_t=int;using io_service_t=int;using kern_return_t=int;
constexpr uint64_t vramBytes=0x400000000ULL,instance=0x10000,address=0x40000;
#define CFSTR(x) x
namespace nvgsp{enum class AdminRpcResult{Success,Refused,Uncertain};}
constexpr uint32_t client=0xc0d00001;
struct Snapshot{uint32_t exception[3],pending,error,core,getput[2],assy[7],armed[7],usage[2];};
static Snapshot live={};static uint32_t crcAssy[2],crcArmed[2];
static std::map<uint64_t,uint32_t> memory;
static uint64_t ticks,phase;
static unsigned allocations,frees,submits;
static bool rejectStart,rejectStop,badMemory,badNotifier,noNotifier,restoreFail,freeFail;
static uint64_t failWrite;
static nvgsp::AdminRpcResult allocResult;
static std::vector<std::vector<uint32_t>> packets;
static std::map<uint32_t,uint32_t> wregs;
static std::vector<uint32_t> fixtureMemory;
static bool unstable,freezeWindow,failInterlockRestore,failGreenArm,ignoreCursorData;
static bool pioReadFail,pioWriteFail,pioWaitAfterPoint;
static std::vector<uint32_t> pioWrites;
bool snapshot(int,Snapshot *s){*s=live;return true;}
void dump(const char*,const Snapshot*){}
bool armed_matches(const Snapshot *s,uint32_t usage,const uint32_t state[7]){
 return s->core==0x200b0000 && s->getput[0]==s->getput[1] && s->usage[0]==usage && s->usage[1]==usage &&
  !memcmp(s->assy,state,28) && !memcmp(s->armed,state,28);}
uint64_t ns_now(){return ticks;}
void usleep(unsigned us){ticks+=uint64_t(us)*1000;}
int peek(int,uint32_t at,unsigned count,uint32_t *out){
 if(at==0x682180 || at==0x68a180){assert(count==2);memcpy(out,at==0x682180?crcAssy:crcArmed,8);return 0;}
 if(at==0x6d8008){if(pioReadFail)return 1;
  if(pioWaitAfterPoint && !pioWrites.empty()){*out=0;return 0;}}
 for(unsigned i=0;i<count;++i)out[i]=wregs[at+i*4];return 0;}
bool property_u64(int,const char* key,uint64_t *out){
 if(!strcmp(key,"NVGspControl-post-init-phase"))*out=phase;
 else if(!strcmp(key,"NVGspControl-dispinst-offset"))*out=instance;
 else if(!strcmp(key,"NVGspControl-wnd-put"))*out=88;
 else *out=0;return true;}
bool vread(int,uint64_t at,uint32_t *out,size_t n){
 if(!fixtureMemory.empty() && at>=address+4096 && at+n*4<=address+4096+fixtureMemory.size()*4){
  memcpy(out,fixtureMemory.data()+(at-address-4096)/4,n*4);return true;}
 for(size_t i=0;i<n;++i)out[i]=memory[at+i*4];return true;}
bool vwrite(int,uint64_t at,const uint32_t *in,size_t n){
 if(at==failWrite || (restoreFail && submits && at==instance+251*8))return false;
 if(!fixtureMemory.empty() && at>=address+4096 && at+n*4<=address+4096+fixtureMemory.size()*4){
  memcpy(fixtureMemory.data()+(at-address-4096)/4,in,n*4);return true;}
 for(size_t i=0;i<n;++i)memory[at+i*4]=in[i];return true;}
nvgsp::AdminRpcResult alloc(int,uint32_t handle,uint32_t cls,const void *params,size_t n,
 uint8_t reply[4096],size_t *bytes){
 ++allocations;assert(handle==0xc0d000f0 && cls==0x40 && n==128);
 auto* p=static_cast<const uint8_t*>(params);uint64_t size;uint32_t attr;
 memcpy(&size,p+64,8);memcpy(&attr,p+24,4);assert((size==4096 || size==4096+16384ULL*2160) && attr==0x10800000);
 if(size>4096)fixtureMemory.resize((size-4096)/4);
 memset(reply,0,4096);*bytes=244;uint64_t at=badMemory?3:address;
 memcpy(reply+196,&at,8);memcpy(reply+180,&size,8);memcpy(reply+140,&attr,4);return allocResult;}
nvgsp::AdminRpcResult release(int,uint32_t handle){assert(handle==0xc0d000f0);++frees;
 assert(!crcArmed[0] && !crcArmed[1]);return freeFail?nvgsp::AdminRpcResult::Refused:nvgsp::AdminRpcResult::Success;}
int IOConnectCallScalarMethod(int,unsigned selector,const uint64_t *in,unsigned count,void*,void*){
 assert(selector==9 && count==2);
 if(in[0]==0x6d8208 || in[0]==0x6d8200){
  if(pioWriteFail)return 1;pioWrites.push_back(uint32_t(in[0]));wregs[uint32_t(in[0])]=uint32_t(in[1]);return 0;}
 assert(in[0]==0x690000);
 const uint32_t target=uint32_t(in[1]);uint32_t at=wregs[0x690004];wregs[0x690000]=target;
 if(freezeWindow)return 0;
 unsigned steps=0;
 while(at!=target){assert(++steps<256);uint32_t header=memory[instance+0x8000+at];
  if((header>>29)==1){at=header&0xffc;continue;}
  const unsigned n=(header>>18)&1023,method=header&0x3ffc;assert(n && at+4+n*4<=4096);at+=4;
  for(unsigned i=0;i<n;++i){wregs[0x690000+method+i*4]=memory[instance+0x8000+at];at+=4;}
 }
 wregs[0x690004]=target;return 0;}
int IOConnectCallStructMethod(int,unsigned selector,const void* in,size_t bytes,void*,void*){
 assert(selector==12);const auto* w=static_cast<const uint32_t*>(in);
 if(bytes==12){assert(w[0]==0x00080218);if(failInterlockRestore)return 1;
  wregs[0x680218]=w[1];wregs[0x68021c]=w[2];return 0;}
 if(bytes==76){
  assert(w[0]==0x00042030 && w[17]==0x00040200 && w[18]==1);
  live.usage[0]=w[1];
  live.assy[0]=w[5];live.assy[1]=w[6];live.assy[2]=w[8];live.assy[3]=w[9];
  live.assy[4]=w[3];live.assy[5]=w[11];live.assy[6]=w[13];
  if(!(failGreenArm && live.assy[2]==0x40)){
   live.usage[1]=live.usage[0];memcpy(live.armed,live.assy,28);
  }return 0;
 }
 assert(bytes==32);wregs[0x680218]=wregs[0x68021c]=0;
 packets.emplace_back(w,w+8);++submits;
 assert(w[0]==0x00082180 && w[3]==0x00080218 && !w[4] && !w[5] && w[6]==0x00040200 && w[7]==1);
 crcAssy[0]=w[1];crcAssy[1]=w[2];
 if(w[1]){
  assert(w[1]==0xc0d000f1 && w[2]==0x30000);
  assert(memory[instance+0x2100]==5 && memory[instance+0x2104]==address>>8);
  if(rejectStart)return 0;
 }else{
  if(rejectStop)return 1;
  if(!noNotifier){memory[address]=badNotifier?0x10009:0x20001;
   const bool visible=live.armed[5]&0x80000000;
   const uint64_t image=(uint64_t(memory[instance+0x20a4])<<8)+(uint64_t(live.armed[2])<<8);
   const uint32_t effect=visible?(ignoreCursorData?0x987:memory[image]):0;
   memory[address+19*4]=0xabc^effect;memory[address+20*4]=0xdef^effect;
   memory[address+21*4]=(0x123^effect)+(unstable?submits:0);}
 }
 memcpy(crcArmed,crcAssy,8);return 0;}
static void reset(){memory.clear();live={};live.core=0x200b0000;
 live.usage[0]=live.usage[1]=0x1110;memset(crcAssy,0,8);memset(crcArmed,0,8);
 ticks=allocations=frees=submits=0;phase=33;packets.clear();
 wregs.clear();fixtureMemory.clear();unstable=freezeWindow=failInterlockRestore=failGreenArm=ignoreCursorData=false;
 pioReadFail=pioWriteFail=pioWaitAfterPoint=false;pioWrites.clear();
 rejectStart=rejectStop=badMemory=badNotifier=noNotifier=restoreFail=freeFail=false;
 failWrite=~0ULL;allocResult=nvgsp::AdminRpcResult::Success;
 memory[instance+123*8]=0xc0d0d001;memory[instance+123*8+4]=0x02400001;
 memory[instance+0x20a0]=0xfeed;memory[instance+0x2160]=0xface;
 const uint32_t dma[5]={5,0,0,0x3ffffff,0};
 for(unsigned i=0;i<5;++i)memory[instance+0x2000+i*4]=dma[i];
 wregs[0x690000]=wregs[0x690004]=88;wregs[0x6902ec]=0x10000;
 wregs[0x690224]=3840|(2160<<16);wregs[0x69022c]=0xcf;wregs[0x690230]=256;wregs[0x690240]=0xc0d0d001;
 wregs[0x681010]=wregs[0x689010]=0x110f00;
 for(unsigned i:{0U,5U,10U})wregs[0x690400+i*4]=0x10000;
 wregs[0x6d8008]=4;wregs[0x68a288]=0xc0d000e1;
}
static void normalWindow(){
 wregs[0x690000]=wregs[0x690004]=140;wregs[0x6902ec]=0x80;wregs[0x6902f4]=0x11;
 wregs[0x690440]=0x40508;wregs[0x690444]=0xc0d000e2;wregs[0x690370]=wregs[0x68021c]=1;
}
'''
SUFFIX = r'''
int main(){
 uint32_t data[1024]={};CrcSample out={};
 assert(!crc_decode(data,&out));
 for(uint32_t bad:{1U,0x10000U,0x10009U,0x10011U,0x10021U,0x800001U}){
  data[0]=bad;assert(!crc_decode(data,&out));}
 data[0]=0x7f0001;data[1019]=1;data[1020]=2;data[1021]=3;
 assert(crc_decode(data,&out) && out.count==127 && out.compositor==1 && out.raster==2 && out.output==3);
 reset();const auto saved=memory;
 assert(trial(0,0)==0 && allocations==1 && frees==1 && submits==2 && memory[instance+0x2100]==0);
 for(const auto& v:saved)assert(memory[v.first]==v.second);
 assert(packets[0][1]==0xc0d000f1 && packets[0][2]==0x30000 && !packets[1][1] && !packets[1][2]);
 reset();phase=32;assert(trial(0,0)==1 && allocations==0 && submits==0);
 reset();live.core=0x200c0000;assert(trial(0,0)==1 && allocations==0);
 reset();crcAssy[1]=1;assert(trial(0,0)==1 && allocations==0);
 reset();memory[instance+251*8]=9;assert(trial(0,0)==1 && allocations==0);
 reset();memory[instance+0x2100]=9;assert(trial(0,0)==1 && allocations==0);
 reset();allocResult=nvgsp::AdminRpcResult::Refused;assert(trial(0,0)==1 && frees==0 && submits==0);
 reset();allocResult=nvgsp::AdminRpcResult::Uncertain;assert(trial(0,0)==1 && frees==0 && submits==0);
 reset();badMemory=true;assert(trial(0,0)==1 && frees==0 && submits==0);
 reset();failWrite=address;assert(trial(0,0)==1 && frees==1 && submits==0);
 reset();failWrite=instance+0x2100;assert(trial(0,0)==1 && frees==0 && submits==0);
 reset();rejectStart=true;assert(trial(0,0)==1 && frees==1 && submits==2 && ticks==200000000);
 reset();rejectStop=true;assert(trial(0,0)==1 && frees==0 && submits==2 && crcArmed[0]==0xc0d000f1);
 reset();badNotifier=true;assert(trial(0,0)==1 && frees==1 && !crcArmed[0]);
 reset();noNotifier=true;assert(trial(0,0)==1 && frees==1 && ticks==220000000);
 reset();restoreFail=true;assert(trial(0,0)==1 && frees==0 && !crcArmed[0]);
 reset();freeFail=true;assert(trial(0,0)==1 && frees==1);
 reset();std::vector<uint32_t> pb(1024);for(unsigned i=0;i<22;++i)memory[instance+0x8000+i*4]=0xfeed0000+i;
 vread(0,instance+0x8000,pb.data(),1024);
 assert(trial(0,0,true)==0 && frees==1 && submits==6 && wregs[0x690260]==0 && wregs[0x690004]==88);
 for(unsigned i=0;i<1024;++i)assert(memory[instance+0x8000+i*4]==pb[i]);
 assert(fixtureMemory.size()==4096U*2160);
 for(size_t i=0;i<fixtureMemory.size();++i){unsigned x=i%4096;uint32_t gray=x*255/3839;
  assert(fixtureMemory[i]==(x<3840?(0xff000000U|gray|(gray<<8)|(gray<<16)):0));}
 reset();unstable=true;assert(trial(0,0,true)==1 && frees==1 && submits==4 && wregs[0x690004]==88);
 reset();wregs[0x690230]=128;assert(trial(0,0,true)==1 && !allocations);
 reset();failWrite=address+4096;assert(trial(0,0,true)==1 && frees==1 && !submits);
 reset();freezeWindow=true;assert(trial(0,0,true)==1 && !frees && !submits && wregs[0x690000]==116);
 reset();wregs[0x690000]=wregs[0x690004]=140;wregs[0x6902ec]=0x80;wregs[0x6902f4]=0x11;
 wregs[0x690440]=0x40508;wregs[0x690444]=0xc0d000e2;wregs[0x690370]=wregs[0x68021c]=1;
 CrcSample normal={};assert(trial(0,0,true,true,&normal)==0 && frees==1 && submits==6);
 assert(normal.output==0x123 && wregs[0x690004]==140 && wregs[0x690370]==1 && wregs[0x68021c]==1);
 reset();wregs[0x690000]=wregs[0x690004]=140;wregs[0x6902ec]=0x80;wregs[0x6902f4]=0x11;
 wregs[0x690440]=0x40508;wregs[0x690444]=0xc0d000e2;wregs[0x690370]=wregs[0x68021c]=1;
 failInterlockRestore=true;assert(trial(0,0,true,true)==1 && !frees && !crcArmed[0]);
 reset();wregs[0x690000]=wregs[0x690004]=140;wregs[0x6902ec]=0x80;wregs[0x6902f4]=0x11;
 wregs[0x690440]=0x40508;wregs[0x690444]=0xc0d000e3;wregs[0x690370]=wregs[0x68021c]=1;
 assert(trial(0,0,true,true)==1 && !allocations);
 reset();normalWindow();bool cursorAccepted=false;
 const auto originalObjects=memory;
 assert(trial(0,0,true,true,nullptr,0x3000000,&cursorAccepted)==0 && cursorAccepted && frees==1 && submits==30);
 assert(live.usage[1]==0x1112 && !live.armed[0] && !(live.armed[5]&0x80000000) && wregs[0x690004]==140);
 for(const auto &v:originalObjects)assert(memory[v.first]==v.second);
 reset();normalWindow();ignoreCursorData=true;
 assert(trial(0,0,true,true,nullptr,0x3000000,&cursorAccepted)==1 && cursorAccepted && frees==1);
 reset();normalWindow();failGreenArm=true;
 assert(trial(0,0,true,true,nullptr,0x3000000,&cursorAccepted)==1 && cursorAccepted && !frees);
 reset();normalWindow();wregs[0x6d8208]=0xffff;
 assert(trial(0,0,true,true,nullptr,0x3000000,&cursorAccepted)==1 && !allocations);
 reset();assert(cursor_pio_point(0,0x00400040) && pioWrites.size()==2 && pioWrites[0]==0x6d8208 && pioWrites[1]==0x6d8200);
 reset();wregs[0x6d8008]=0;assert(!cursor_pio_point(0,0x00400040) && pioWrites.empty() && ticks==100000000);
 reset();pioReadFail=true;assert(!cursor_pio_point(0,0x00400040) && pioWrites.empty());
 reset();pioWriteFail=true;assert(!cursor_pio_point(0,0x00400040) && pioWrites.empty());
 reset();pioWaitAfterPoint=true;assert(!cursor_pio_point(0,0x00400040) && pioWrites.size()==1 && ticks==100000000);
 puts("actual CRC/fixture: notifier bounds, guarded failures, all ramp pixels, three-capture stability and exact window/PB restoration PASS");
}
'''

def main():
    header = (ROOT/'tools/display/nvdisplay_crc_probe.hpp').read_text()
    header = header[header.index('struct CrcSample'):]
    cpp = (ROOT/'tools/display/nvdisplay_crc_resource.hpp').read_text()
    trial = cpp[cpp.index('static constexpr'):].replace('crc_resource_trial(', 'trial(')
    window = (ROOT/'tools/display/nvdisplay_window_probe.hpp').read_text()
    window = window[window.index('struct WindowProbeState'):]
    window = window[:window.index('static inline bool window_queue(')] + window[window.index('static inline bool window_rewind('):]
    fixture = (ROOT/'tools/display/nvdisplay_fixture_probe.hpp').read_text()
    fixture = fixture[fixture.index('static constexpr'):]
    cursor=(ROOT/'tools/display/nvdisplay_cursor_probe.hpp').read_text()
    cursor=cursor[cursor.index('enum class CursorObservation'):]
    observer=(ROOT/'tools/display/nvdisplay_cursor_crc_probe.hpp').read_text()
    observer=observer[observer.index('struct CursorCrcContext'):]
    pio=(ROOT/'tools/display/nvdisplay_cursor_pio_probe.hpp').read_text()
    pio=pio[pio.index('static inline bool cursor_pio_free('):]
    common=(ROOT/'tools/display/nvcursor_common.h').read_text()
    core=common[common.index('static inline kern_return_t core('):common.index('static inline uint64_t ns_now(')]
    wait=common[common.index('static inline bool wait_armed('):common.index('static inline bool property_u64(')]
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        (p/'check.cpp').write_text(PREFIX+core+wait+window+header+fixture+cursor+observer+pio+trial+SUFFIX)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(p/'check.cpp'), '-o', str(p/'check')], check=True)
        subprocess.run([str(p/'check')], check=True)

if __name__ == '__main__':
    main()
