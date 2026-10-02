#!/usr/bin/env python3
"""Compile actual cursorImage against a bounded cursor-DMA mock.

Old absolute OFFSET points beyond the 32KiB DMA object; both new buffers must
address the pixels uploaded to VRAM. Error/disable behavior is checked too.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
prefix = r'''
#include <cstdint>
#include <cstdio>
#include <vector>
#include <cassert>
using UInt32=uint32_t; using UInt64=uint64_t; using IOReturn=int;
constexpr int kIOReturnSuccess=0, kIOReturnIOError=1;
constexpr UInt32 kCursorCtxdma=0xc0d0d002;
static UInt32 liveUsage=0x1110;
struct IOMemoryMap { UInt64 getLength(){return 0x700000;} void release(){} };
IOMemoryMap* sharedBar0Map(void*){static IOMemoryMap m;return &m;}
struct Bar0Io { IOMemoryMap* map; bool read(UInt32 at,UInt32 *out){assert(at==0x68a030);*out=liveUsage;return true;} };
void IODelay(unsigned){}
struct NVGspControl {
 UInt32 cursorBuf_=0,cursorControl_=0; UInt64 cursorBase_=0x1aab0000;
 bool cursorVisible_=true; void* pci_=nullptr;
 bool fault=false,uploadFail=false; unsigned submits=0; UInt64 uploaded=0;
 UInt32 expectedUsage=0;
 std::vector<UInt32> packet;
 IOReturn cursorSetup(){return 0;}
 IOReturn vramAccess(UInt64 addr,UInt32*,unsigned,bool){
  if(uploadFail)return 1; if(!uploaded)uploaded=addr; return 0;
 }
 IOReturn submitCore(const UInt32* words,unsigned n){
  ++submits; packet.assign(words,words+n);
  if(submits==1 || submits==2 || submits==3){
   bool both=false;
   for(unsigned i=0;i+2<n;++i) if(words[i]==((2U<<18)|0x2088))
    both=words[i+1]==kCursorCtxdma && words[i+2]==kCursorCtxdma;
   if(!both)return 1;
  }
  for(unsigned i=0;i+1<n;++i) if(words[i]==((2U<<18)|0x2090)){
   if(i+2>=n || words[i+1]!=words[i+2])return 1;
   UInt64 relative=UInt64(words[i+1])<<8;
   if(relative+0x4000>0x8000 || cursorBase_+relative!=uploaded){
    std::fprintf(stderr,"cursor OFFSET addresses wrong/out-of-bounds pixels\n");return 1;
   }
  }
  return 0;
 }
 bool coreException(){bool value=fault;fault=false;return value;}
 IOReturn waitCursorArmed(UInt32 usage,const UInt32 state[7]){
  expectedUsage=usage;assert(state[0]==kCursorCtxdma && state[1]==kCursorCtxdma);
  assert(state[2]==state[3] && cursorBase_+(UInt64(state[2])<<8)==uploaded);
  bool value=fault;fault=false;return value?1:0;
 }
 void setProperty(const char*,UInt32,unsigned){}
 IOReturn cursorImage(const UInt32*,UInt32,UInt32);
};
'''
suffix = r'''
int main(){
 UInt32 image[4096]{}; NVGspControl d;
 if(d.cursorImage(image,0,0)!=0 || d.cursorBuf_!=1)return 12;
 d.uploaded=0;
 if(d.cursorImage(image,3,4)!=0 || d.cursorBuf_!=0)return 13;
 d.uploaded=0;d.fault=true;
 if(d.cursorImage(image,0,0)!=1 || d.cursorVisible_ || d.submits!=4 || d.cursorBuf_!=0 || d.cursorControl_)return 14;
 if(d.packet.size()<2 || (d.packet[1]&0x80000000U))return 15;
 NVGspControl failed;failed.uploadFail=true;
 if(failed.cursorImage(image,0,0)!=1 || failed.submits)return 16;
 NVGspControl maxCapacity;liveUsage=0x1114;
 if(maxCapacity.cursorImage(image,0,0)!=0 || maxCapacity.expectedUsage!=0x1114)return 17;
 std::puts("actual cursorImage: DMA-relative buffers, upload/ARM failure cleanup and larger capacity preservation PASS");
}
'''
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT/'drivers/NVGspControl/NVGspControl.cpp')
    args = ap.parse_args()
    source = args.source.read_text()
    method = source[source.index('IOReturn NVGspControl::cursorImage('):source.index('IOReturn NVGspControl::cursorShow(')]
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'check.cpp').write_text(prefix+method+suffix)
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=undefined',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)


if __name__ == '__main__':
    main()
