#!/usr/bin/env python3
"""Run actual LUT setup methods against independent encoding/DMA readback."""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "drivers/NVGspCore/NVGspDisplayLut.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <strings.h>
using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnIOError=1,kIOReturnNotReady=2,kIOReturnNoMemory=3;
constexpr UInt32 kOlutCtxdma=OLUT_HANDLE,kIlutCtxdma=ILUT_HANDLE;
static bool allocationFail=false; static unsigned allocations=0;
void* IOMalloc(size_t n){if(allocationFail)return nullptr;++allocations;return malloc(n);}
void IOFree(void* p,size_t){--allocations;free(p);}
static std::map<UInt64,UInt32> objects;
bool praminWriteWords(void*,UInt64 at,const UInt32* w,UInt32 count){
 for(unsigned i=0;i<count;++i)objects[at+i*4]=w[i];return true;
}
struct NVGspControl {
 UInt64 dispInstOffset_=0x10000,scratchOffset_=0x10000000;void* pci_=nullptr;
 unsigned uploadCalls=0,failCall=0;std::map<UInt64,UInt32> pixels;
 void lk(unsigned){} void ulk(){}
 IOReturn vramAccess(UInt64 at,UInt32* w,UInt32 count,bool){
  if(++uploadCalls==failCall)return kIOReturnIOError;
  for(unsigned i=0;i<count;++i)pixels[at+i*4]=w[i];return 0;
 }
 IOReturn olutSetup(); IOReturn ilutSetup();
};
static double halfValue(uint16_t bits){
 unsigned e=(bits>>10)&31,m=bits&1023;
 return e?std::ldexp(1.0+m/1024.0,int(e)-15):std::ldexp(double(m),-24);
}
'''
HEAD_CHECK = r'''
struct Mode {
 bool valid=true,olut=true,cursor64=false;
 UInt32 polarity=0,pclkHz=0,hActive=0,vActive=0,rasterSize=0,syncEnd=0,
 blankEnd=0,blankStart=0,minFrameIdle=0;
};
static void applyHead(Mode mode_,UInt32 *vals){
 constexpr auto kHeads=sizeof(kHeadAddrs)/sizeof(kHeadAddrs[0]);
HEAD_OVERRIDE
}
static void checkHead(){
 constexpr auto n=sizeof(kHeadAddrs)/sizeof(kHeadAddrs[0]);
 UInt32 values[n];
 for(bool enabled:{false,true}){
  Mode m;m.olut=enabled;
  for(auto &v:values)v=0x123456;
  applyHead(m,values);
  unsigned coefficients=0;
  for(unsigned i=0;i<n;++i){
   const auto method=kHeadAddrs[i];
   if(method>=0x2244 && method<=0x2270){
    ++coefficients;
    const auto index=(method-0x2244)/4;
    const bool diagonal=(index/4)==(index%4);
    assert(values[i]==(enabled?(diagonal?65536U:0U):0x123456U));
    // Hardware S5.14 coefficient lives in register bits 20:2.
    if(enabled)assert(double(values[i]>>2)/16384.0==(diagonal?1.0:0.0));
   }
   if(method==0x2240)assert(values[i]==(enabled?1U:0x123456U));
   if(method==0x2288)assert(values[i]==(enabled?kOlutCtxdma:0x123456U));
  }
  assert(coefficients==12);
 }
}
'''
SUFFIX = r'''
int main(){
 checkHead();
 NVGspControl d;
 for(unsigned slot:{56,60}){
  objects[d.dispInstOffset_+slot*8]=0xc0d0d002;
  objects[d.dispInstOffset_+slot*8+4]=0x00414001;
 }
 assert(d.olutSetup()==0 && d.ilutSetup()==0 && allocations==0);
 for(unsigned slot:{56,60}){
  assert(objects[d.dispInstOffset_+slot*8]==0xc0d0d002);
  assert(objects[d.dispInstOffset_+slot*8+4]==0x00414001);
 }
 const auto output=d.scratchOffset_+nvgsp::kOlutScratchDelta;
 const auto input=d.scratchOffset_+nvgsp::kIlutScratchDelta;
 for(unsigned i=0;i<8;++i)assert(d.pixels[output+i*4]==0 && d.pixels[input+i*4]==0);
 for(unsigned i=0;i<1025;++i){
  const auto at=(4+i)*8;
  const double expected=double(i<1024?i:1023)/1024.0;
  const auto o=d.pixels[output+at],a=d.pixels[input+at];
  assert(double(o&0xffff)/65536.0==expected && halfValue(a&0xffff)==expected);
  assert(o>>16==(o&0xffff) && d.pixels[output+at+4]==(o&0xffff));
  assert(a>>16==(a&0xffff) && d.pixels[input+at+4]==(a&0xffff));
 }
 assert(objects[d.dispInstOffset_+0x20c4]==output>>8);
 assert(objects[d.dispInstOffset_+0x20e4]==input>>8);
 assert(objects[d.dispInstOffset_+0x20cc]==(output+0x2fff)>>8);
 assert(objects[d.dispInstOffset_+0x20ec]==(input+0x2fff)>>8);
 for(bool in:{false,true}){
  for(unsigned fail=1;fail<=3;++fail){
   NVGspControl f;f.failCall=fail;objects.clear();
   assert((in?f.ilutSetup():f.olutSetup())==kIOReturnIOError);
   assert(objects.empty() && allocations==0);
  }
  NVGspControl f;allocationFail=true;objects.clear();
  assert((in?f.ilutSetup():f.olutSetup())==kIOReturnNoMemory);
  assert(objects.empty() && !f.uploadCalls && allocations==0);allocationFail=false;
 }
 puts("actual LUT setup: fixed/half identity, separate buffers, cursor hash preservation, full OCSC identity and upload failures PASS");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'drivers/NVGspControl/NVGspControl.cpp')
    source = parser.parse_args().source.read_text()
    handles = [re.search(r'k'+name+r'Ctxdma\s*=\s*(0x[0-9a-fA-F]+)', source).group(1)
               for name in ('Olut', 'Ilut')]
    prefix = PREFIX.replace('OLUT_HANDLE', handles[0]).replace('ILUT_HANDLE', handles[1])
    methods = source[source.index('IOReturn NVGspControl::olutSetup()'):
                     source.index('IOReturn NVGspControl::cursorSetup()')]
    head_array = re.search(r'constexpr UInt32 kHeadAddrs\[\] = \{.*?\};', source, re.S).group(0)
    modeset = source[source.index('IOReturn NVGspControl::modesetHead0('):]
    override = modeset[modeset.index('    if (mode_.valid) {'):
                       modeset.index('    // 0.138.2:')]
    head = head_array+'\n'+HEAD_CHECK.replace('HEAD_OVERRIDE', override)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path/'check.cpp').write_text(prefix+methods+head+SUFFIX)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-I', str(ROOT), str(path/'check.cpp'),
                        '-o', str(path/'check')], check=True)
        subprocess.run([str(path/'check')], check=True)


if __name__ == '__main__':
    main()
