#!/usr/bin/env python3
"""Execute fixed cursor comparison and actual joint-restoration packet builder."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
using io_connect_t=int;using kern_return_t=int;
constexpr uint64_t vramBytes=0x400000000ULL,instance=0x10000,image=0x80000;
struct Snapshot{uint32_t assy[7],armed[7],usage[2];};
static Snapshot live={};static std::map<uint64_t,uint32_t> memory;
static bool reject=false,failUpload=false,failPrepare=false;
static std::vector<uint32_t> usages,controls;
bool snapshot(int,Snapshot *s){*s=live;return true;}
bool armed_matches(const Snapshot *s,uint32_t usage,const uint32_t state[7]){
 return s->usage[0]==usage && s->usage[1]==usage && !memcmp(s->assy,state,28) && !memcmp(s->armed,state,28);
}
int peek(int,uint32_t at,unsigned count,uint32_t *out){
 assert(count==1);*out=at==0x6902ec?0x80:0xc0d000e1;return 0;
}
bool vread(int,uint64_t at,uint32_t *out,size_t count){
 for(size_t i=0;i<count;++i)out[i]=memory[at+i*4];return true;
}
bool vwrite(int,uint64_t at,const uint32_t *in,size_t count){
 if(failUpload && at>=image && at<image+0x8000)return false;
 for(size_t i=0;i<count;++i)memory[at+i*4]=in[i];return true;
}
int core(int,uint32_t usage,const uint32_t state[7]){
 usages.push_back(usage);controls.push_back(state[5]);
 live.usage[0]=usage;memcpy(live.assy,state,28);
 if(!(reject && (state[5]&0x80000000)) && !(failPrepare && state[5]==0xe9 && (usage&7))){
  live.usage[1]=usage;memcpy(live.armed,state,28);
 }
 return 0;
}
bool wait_armed(int,const char*,const Snapshot*,uint32_t usage,const uint32_t state[7]){
 return armed_matches(&live,usage,state);
}
static std::vector<uint32_t> packet;
int IOConnectCallStructMethod(int,unsigned selector,const void *in,size_t bytes,void*,void*){
 assert(selector==12);const auto *w=static_cast<const uint32_t*>(in);
 packet.assign(w,w+bytes/4);return 0;
}
static Snapshot initial(){
 Snapshot s={};s.usage[0]=s.usage[1]=0x1110;s.assy[5]=s.armed[5]=0xe9;s.assy[6]=s.armed[6]=0x2ff;return s;
}
struct LutState{Snapshot display;};
static void reset(){
 live=initial();memory.clear();usages.clear();controls.clear();
 for(unsigned slot:{56U,60U}){
  memory[instance+slot*8]=0xc0d0d002;memory[instance+slot*8+4]=0x00414001;
 }
 // Canary unrelated objects and LUT descriptors, preserved by cursor writes.
 memory[instance+0x20c0]=0xfeed;
 failUpload=failPrepare=reject=false;
}
'''
SUFFIX = r'''
static unsigned observationCalls;
static CursorObservation observerResult;
static CursorObservation observe(int,const Snapshot &s,bool visible,void*){
 ++observationCalls;
 assert(armed_matches(&s,0x1112,s.armed) && bool(s.armed[5]&0x80000000)==visible);
 return observerResult;
}
int main(){
 const auto original=initial();bool accepted=false;
 reset();reject=true;
 assert(cursor_compare(0,instance,image,original,&accepted) && !accepted);
 assert(armed_matches(&live,0x1110,original.armed) && usages.size()==2);
 assert(usages[0]==0x1112 && usages[1]==0x1110 && controls[0]==0x800001cf);
 reset();
 assert(cursor_compare(0,instance,image,original,&accepted) && accepted);
 assert(usages.size()==3 && usages[0]==0x1112 && usages[1]==0x1112 && usages[2]==0x1112);
 assert(controls[0]==0x800001cf && controls[1]==0x1cf && controls[2]==0xe9);
 assert(armed_matches(&live,0x1112,original.armed));
 assert(memory[instance+0x20a4]==image>>8 && memory[instance+0x20ac]==(image+0x7fff)>>8);
 assert(memory[image]==0xffff0000 && memory[image+0x3ffc]==0xffff0000);
 reset();memory[instance+56*8]=memory[instance+56*8+4]=0;
 memory[instance+60*8]=memory[instance+60*8+4]=0;
 assert(cursor_compare(0,instance,image,original,&accepted) && accepted);
 assert(memory[instance+56*8]==0xc0d0d002 && memory[instance+60*8+4]==0x00414001);
 reset();memory[instance+56*8]=0xabcdef;
 assert(!cursor_compare(0,instance,image,original,&accepted) && usages.empty());
 reset();failUpload=true;assert(!cursor_compare(0,instance,image,original,&accepted) && usages.empty());
 reset();failPrepare=true;assert(!cursor_compare(0,instance,image,original,&accepted) && accepted);
 reset();assert(!cursor_compare(0,instance,image+4,original,&accepted) && usages.empty());
 reset();observationCalls=0;observerResult=CursorObservation::Accepted;
 assert(cursor_compare(0,instance,image,original,&accepted,observe) && accepted && observationCalls==2);
 reset();observationCalls=0;observerResult=CursorObservation::Refused;
 assert(cursor_compare(0,instance,image,original,&accepted,observe) && accepted && observationCalls==2 && usages.size()==3);
 reset();observationCalls=0;observerResult=CursorObservation::Uncertain;
 assert(!cursor_compare(0,instance,image,original,&accepted,observe) && accepted && observationCalls==1 && usages.size()==1);
 const uint32_t lut[4]={0,0xcccccccc,0,0};const uint32_t ocsc[13]={};
 assert(submit(0,lut,ocsc,true,&original)==0 && packet.size()==38);
 std::map<uint32_t,uint32_t> methods;
 for(size_t p=0;p<packet.size();){
  const auto header=packet[p++],count=(header>>18)&1023,method=header&0x3ffc;
  assert(!(header>>29) && count && p+count<=packet.size());
  for(unsigned i=0;i<count;++i)methods[method+i*4]=packet[p++];
 }
 assert(methods[0x2030]==0x1110 && methods[0x209c]==0xe9 && methods[0x20a0]==0x2ff);
 assert(!methods[0x2088] && !methods[0x208c] && methods[0x2284]==0xcccccccc);
 assert(methods[0x218]==0 && methods[0x21c]==1 && methods[0x200]==1);
 assert(submit(0,lut,ocsc,false)==0 && packet.size()==24);
 for(bool enabled:{false,true}){
  LutState before={};before.display=original;const bool cursorAccepted=enabled;
RESTORE_EXPECTED
  assert(restoreExpected.display.usage[1]==(enabled?0x1112U:0x1110U));
  assert(restoreExpected.display.usage[0]==restoreExpected.display.usage[1]);
  assert(!memcmp(restoreExpected.display.armed,original.armed,28));
 }
 puts("actual fixed cursor: accepted/rejected cleanup, retained bounds/failures and joint packet encoding PASS");
}
'''


def main():
    header = (ROOT/'tools/display/nvdisplay_cursor_probe.hpp').read_text()
    header = header.replace('#pragma once', '').replace('#include "nvdisplay_admin.hpp"', '')
    source = (ROOT/'tools/display/nvdisplay_olut_probe.cpp').read_text()
    submit = source[source.index('static kern_return_t submit('):source.index('static int trial(')]
    restore = source[source.index('    LutState restoreExpected = before;'):
                     source.index('    const auto restore = submit(')]
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path/'check.cpp').write_text(PREFIX+header+submit+SUFFIX.replace('RESTORE_EXPECTED', restore))
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(path/'check.cpp'), '-o', str(path/'check')], check=True)
        subprocess.run([str(path/'check')], check=True)


if __name__ == '__main__':
    main()
