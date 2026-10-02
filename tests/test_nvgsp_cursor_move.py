#!/usr/bin/env python3
"""Run the real cursorMove method against failed and delayed PIO accesses."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using UInt32=uint32_t;using UInt64=uint64_t;using SInt32=int32_t;using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNoMemory=2,
 kIOReturnIOError=3,kIOReturnTimeout=4;
static unsigned reads,sleeps,maps,releases;
static bool noMap;
static UInt64 length=0x6d9000;
static unsigned failRead,failWrite,initialWait,afterPointWait;
static UInt32 freeValue=4;
static bool pointDone;
static std::vector<UInt32> methods,values;
static int setupResult,imageResult,showResult;
static UInt32 property;
static unsigned imageCalls,allocated,freed;
static bool noAlloc;
void* IOMalloc(unsigned n){if(noAlloc)return nullptr;++allocated;return std::malloc(n);}
void IOFree(void* p,unsigned n){assert(n==64*64*4);++freed;std::free(p);}
struct IOMemoryMap{UInt64 getLength(){return length;}void release(){++releases;}};
IOMemoryMap* sharedBar0Map(void*){if(noMap)return nullptr;++maps;static IOMemoryMap m;return &m;}
struct Bar0Io{
 IOMemoryMap* map;
 bool read(UInt32 at,UInt32* v){assert(at==0x6d8008);++reads;
  if(reads==failRead)return false;
  unsigned& wait=pointDone?afterPointWait:initialWait;
  if(wait){--wait;*v=0;}else *v=freeValue;
  return true;}
 bool write(UInt32 at,UInt32 v){methods.push_back(at);values.push_back(v);
  if(methods.size()==failWrite)return false;
  if(at==0x6d8208)pointDone=true;
  return true;}
};
void IOSleep(unsigned ms){assert(ms==1);++sleeps;}
void IODelay(unsigned){assert(false && "must yield during a bounded PIO wait");}
struct NVGspControl{bool cursorReady_=true,cursorVisible_=false;void* pci_=reinterpret_cast<void*>(1);
 IOReturn cursorMove(SInt32,SInt32);IOReturn cursorTest(UInt32);
 IOReturn cursorSetup(){return setupResult;}
 IOReturn cursorShow(bool visible){assert(!visible);return showResult;}
 IOReturn cursorImage(UInt32* p,int x,int y){assert(x==0&&y==0);++imageCalls;
  assert(p[0]==0xffff0000 && p[32*64+32]==0x800000ff);return imageResult;}
 void setProperty(const char*,UInt32 v,int bits){assert(bits==32);property=v;}
};
void reset(){reads=sleeps=maps=releases=0;noMap=false;length=0x6d9000;
 failRead=failWrite=initialWait=afterPointWait=0;freeValue=4;pointDone=false;
 methods.clear();values.clear();setupResult=imageResult=showResult=0;property=~0U;
 imageCalls=allocated=freed=0;noAlloc=false;}
'''
SUFFIX = r'''
int main(){NVGspControl d;
 reset();assert(d.cursorMove(-2,32767)==kIOReturnSuccess);
 assert((methods==std::vector<UInt32>{0x6d8208,0x6d8200}));
 assert(values[0]==0x7ffffffe && values[1]==0 && reads==2 && sleeps==0 && releases==1);
 reset();initialWait=7;afterPointWait=9;
 assert(d.cursorMove(64,64)==kIOReturnSuccess && reads==18 && sleeps==16 && releases==1);
 reset();d.cursorReady_=false;assert(d.cursorMove(0,0)==kIOReturnNotReady && maps==0);d.cursorReady_=true;
 reset();d.pci_=nullptr;assert(d.cursorMove(0,0)==kIOReturnNotReady && maps==0);d.pci_=reinterpret_cast<void*>(1);
 reset();noMap=true;assert(d.cursorMove(0,0)==kIOReturnNoMemory && releases==0);
 reset();length=0x6d8fff;assert(d.cursorMove(0,0)==kIOReturnNoMemory && releases==1 && methods.empty());
 reset();failRead=1;assert(d.cursorMove(0,0)==kIOReturnIOError && methods.empty() && sleeps==0 && releases==1);
 reset();failRead=2;assert(d.cursorMove(0,0)==kIOReturnIOError && methods.size()==1 && releases==1);
 reset();failWrite=1;assert(d.cursorMove(0,0)==kIOReturnIOError && methods.size()==1 && reads==1 && releases==1);
 reset();failWrite=2;assert(d.cursorMove(0,0)==kIOReturnIOError && methods.size()==2 && releases==1);
 reset();freeValue=0;assert(d.cursorMove(0,0)==kIOReturnTimeout && methods.empty() && reads==100 && sleeps==100 && releases==1);
 reset();freeValue=0x80000000;assert(d.cursorMove(0,0)==kIOReturnTimeout && methods.empty() && sleeps==100 && releases==1);
 reset();afterPointWait=100;assert(d.cursorMove(0,0)==kIOReturnTimeout && methods.size()==1 && reads==101 && sleeps==100 && releases==1);
 reset();initialWait=99;afterPointWait=99;
 assert(d.cursorMove(65537,-65537)==kIOReturnSuccess && reads==200 && sleeps==198 && releases==1);
 assert(values[0]==0xffff0001 && values[1]==0);
 reset();assert(d.cursorTest(1)==0 && property==0 && imageCalls==1 && allocated==1 && freed==1 && values[0]==0x00400040);
 reset();failRead=1;assert(d.cursorTest(1)==kIOReturnIOError && property==kIOReturnIOError && methods.empty() && freed==1);
 reset();afterPointWait=100;assert(d.cursorTest(1)==kIOReturnTimeout && property==kIOReturnTimeout && methods.size()==1 && freed==1);
 reset();imageResult=kIOReturnIOError;assert(d.cursorTest(1)==kIOReturnIOError && property==kIOReturnIOError && maps==0 && freed==1);
 reset();setupResult=kIOReturnNotReady;assert(d.cursorTest(1)==kIOReturnNotReady && allocated==0 && maps==0);
 reset();assert(d.cursorTest(0)==0 && allocated==0 && maps==0);
 reset();showResult=kIOReturnIOError;assert(d.cursorTest(2)==kIOReturnIOError && allocated==0 && maps==0);
 reset();noAlloc=true;assert(d.cursorTest(1)==kIOReturnNoMemory && freed==0 && maps==0);
 puts("cursorMove: two PIO guards, bounded yield, status/mask/write failures and release PASS");
}
'''

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, default=ROOT/'drivers/NVGspControl/NVGspControl.cpp')
    args = ap.parse_args()
    source = args.source.read_text()
    methods = []
    for name in ['cursorMove', 'cursorTest']:
        begin = source.index('IOReturn NVGspControl::' + name + '(')
        end = source.index('\n}', begin) + 2
        methods.append(source[begin:end])
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        (p/'check.cpp').write_text(PREFIX + '\n'.join(methods) + SUFFIX)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(p/'check.cpp'), '-o', str(p/'check')], check=True)
        subprocess.run([str(p/'check')], check=True)

if __name__ == '__main__':
    main()
