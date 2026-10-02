#!/usr/bin/env python3
"""Execute the actual bounded window helper against a C67E PB interpreter."""
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
using io_connect_t=int;using io_service_t=int;
#define CFSTR(x) x
static constexpr uint64_t inst=0x10000;
static uint32_t pb[1024];static std::map<uint32_t,uint32_t> regs;
static unsigned pokes=0;static bool failWrite=false;
bool property_u64(int,const char *key,uint64_t *v){
 if(strcmp(key,"NVGspControl-wnd-put"))return false;*v=88;return true;
}
int peek(int,uint32_t at,unsigned count,uint32_t *out){
 for(unsigned i=0;i<count;++i)out[i]=regs[at+4*i];return 0;
}
bool vread(int,uint64_t at,uint32_t *out,size_t count){
 assert(at>=inst+0x8000 && at+count*4<=inst+0x9000);
 memcpy(out,pb+(at-inst-0x8000)/4,count*4);return true;
}
bool vwrite(int,uint64_t at,const uint32_t *in,size_t count){
 if(failWrite)return false;
 assert(at>=inst+0x8000 && at+count*4<=inst+0x9000);
 memcpy(pb+(at-inst-0x8000)/4,in,count*4);return true;
}
int IOConnectCallScalarMethod(int,unsigned selector,const uint64_t *in,
 unsigned n,void*,void*){
 assert(selector==9 && n==2 && in[0]==0x690000 && !(in[1]&3) && in[1]<4096);
 ++pokes;const auto put=uint32_t(in[1]);auto get=regs[0x690004];unsigned steps=0;
 while(get!=put){
  assert(++steps<100);const auto word=pb[get/4];
  if(word>>29==1){assert(!(word&~0x20000ffc));get=word&0xffc;continue;}
  assert(word>>29==0);const auto count=(word>>18)&1023,method=word&0x3ffc;
  assert(count && get+(count+1)*4<=4096);
  for(unsigned i=0;i<count;++i)regs[0x690000+method+i*4]=pb[get/4+i+1];
  get+=(count+1)*4;
 }
 regs[0x690000]=put;regs[0x690004]=get;return 0;
}
uint64_t ns_now(){static uint64_t n=0;return n+=1000000;}
void usleep(unsigned){}
'''
SUFFIX = r'''
int main(){
 regs[0x690000]=regs[0x690004]=88;regs[0x6902ec]=0x10000;
 regs[0x681010]=regs[0x689010]=0x110f00;regs[0x69022c]=0xcf;
 for(unsigned i:{0U,5U,10U})regs[0x690400+i*4]=0x10000;
 for(unsigned i=0;i<22;++i)pb[i]=0xfeed0000+i;
 WindowProbeState before={};uint32_t saved[1024];
 assert(window_guard(0,0,inst,&before,saved));
 pb[51]=1;assert(!window_guard(0,0,inst,&before,saved));pb[51]=0;
 assert(window_guard(0,0,inst,&before,saved));
 auto changed=before;changed.composition=0x80;changed.factor=0x11;
 changed.ilut[0]=0x40508;changed.ilut[1]=0xc0d000e2;
 uint32_t next=0;failWrite=true;
 assert(!window_queue(0,inst,88,changed,&next) && !pokes);failWrite=false;
 assert(!window_queue(0,inst,92,changed,&next) && !pokes);
 assert(window_queue(0,inst,88,changed,&next) && next==140 && pokes==1);
 WindowProbeState now={};assert(window_snapshot(0,&now) && window_equal(now,changed));
 assert(!window_rewind(0,inst,next,before,saved) && pokes==1);
 assert(window_queue(0,inst,next,before,&next) && next==192);
 assert(window_rewind(0,inst,next,before,saved));
 assert(!memcmp(pb,saved,sizeof(pb)) && regs[0x690000]==88 && regs[0x690004]==88);
 assert(!regs[0x690370] && !regs[0x690374] && pokes==4);
 puts("actual window helper: guard/failure, fixed composition packet, exact PUT rewind and all PB bytes PASS");
}
'''


def main():
    header = (ROOT/'tools/display/nvdisplay_window_probe.hpp').read_text()
    header = header.replace('#pragma once', '').replace('#include "nvdisplay_admin.hpp"', '')
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path/'check.cpp').write_text(PREFIX+header+SUFFIX)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(path/'check.cpp'), '-o', str(path/'check')], check=True)
        subprocess.run([str(path/'check')], check=True)


if __name__ == '__main__':
    main()
