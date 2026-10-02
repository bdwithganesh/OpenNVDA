#!/usr/bin/env python3
"""Exercise actual cursor ARM polling and show/hide against register snapshots.

Fetching a packet, matching ASSY or an ACKed exception must not substitute for
matching idle ARMED state. This harness executes the source methods unchanged.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using UInt32=uint32_t; using UInt64=uint64_t; using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNoMemory=2,
 kIOReturnTimeout=3,kIOReturnIOError=4,kIOReturnNotReady=5;
static std::map<UInt32,UInt32> regs;
static unsigned sleeps, maps, releases, acknowledgements;
static bool noMap;
static UInt64 mapLength;
static UInt32 failRead;
static int armAfter, faultAfter;
static UInt32 wantUsage=0x1112;
static const UInt32 want[7]={0xc0d0d002,0xc0d0d002,0x40,0x40,0,0x800001cf,0x75ff};
struct IOMemoryMap {
 UInt64 getLength(){return mapLength;}
 void release(){++releases;}
};
IOMemoryMap* sharedBar0Map(void*){if(noMap)return nullptr;++maps;static IOMemoryMap m;return &m;}
struct Bar0Io {
 IOMemoryMap* map;
 bool read(UInt32 at,UInt32* value){if(at==failRead)return false;*value=regs[at];return true;}
 bool write(UInt32 at,UInt32 value){assert(at==0x611020 && value==0x90000000);
  ++acknowledgements;regs[at]=value;regs[0x611854]=0;return true;}
};
struct OSData {
 std::vector<UInt32> words;
 static OSData* withBytes(const void* ptr,unsigned size){auto* d=new OSData;
  auto* p=static_cast<const UInt32*>(ptr);d->words.assign(p,p+size/4);return d;}
 void release(){delete this;}
};
static void arm(){regs[0x68a030]=regs[0x682030];
 for(unsigned i=0;i<7;++i)regs[0x68a088+i*4]=regs[0x682088+i*4];}
void IOSleep(unsigned n){assert(n==1);++sleeps;
 if(armAfter>=0 && sleeps==unsigned(armAfter))arm();
 if(faultAfter>=0 && sleeps==unsigned(faultAfter))regs[0x611854]=1;
}
struct NVGspControl {
 void* pci_=reinterpret_cast<void*>(1); bool cursorReady_=true,cursorVisible_=false;
 UInt32 cursorControl_=want[5]; unsigned submits=0;
 int submitMode=0; // 0 accepted, 1 fetched-only then cleanup, 2 failed submissions
 std::map<std::string,UInt32> props; std::vector<UInt32> exception;
 void setProperty(const char* name,UInt32 value,unsigned bits){assert(bits==32);props[name]=value;}
 void setProperty(const char*,OSData* data){exception=data->words;}
 IOReturn peekBar0(UInt32 at,unsigned n,UInt32* out){IOMemoryMap m;Bar0Io b{&m};
  for(unsigned i=0;i<n;++i)if(!b.read(at+i*4,out+i))return kIOReturnIOError;return 0;}
 IOReturn submitCore(const UInt32* words,unsigned n){++submits;assert(n==7);
  assert(words[0]==((1U<<18)|0x209c) && words[2]==((2U<<18)|0x218));
  assert(words[3]==0 && words[4]==0 && words[5]==((1U<<18)|0x200) && words[6]==1);
  if(submitMode==2)return kIOReturnIOError;
  regs[0x68209c]=words[1];
  if(submitMode==0 || submits>1)arm();return 0;}
 UInt32 coreException();
 IOReturn waitCursorArmed(UInt32,const UInt32[7]);
 IOReturn cursorShow(bool);
};
static void reset(bool matching=true){regs.clear();sleeps=maps=releases=acknowledgements=0;
 noMap=false;mapLength=0x700000;failRead=0xffffffff;armAfter=faultAfter=-1;
 regs[0x610630]=0x200b0000;regs[0x680000]=regs[0x680004]=0x74;
 regs[0x682030]=wantUsage;regs[0x68a030]=matching?wantUsage:0x1110;
 for(unsigned i=0;i<7;++i){regs[0x682088+i*4]=want[i];regs[0x68a088+i*4]=matching?want[i]:0;}
}
static void balanced(){assert(maps==releases);}
'''
SUFFIX = r'''
int main(){
 reset();NVGspControl d;
 assert(d.waitCursorArmed(wantUsage,want)==0 && sleeps==0);balanced();
 reset(false);assert(d.waitCursorArmed(wantUsage,want)==kIOReturnTimeout && sleeps==200);balanced();
 reset(false);armAfter=7;assert(d.waitCursorArmed(wantUsage,want)==0 && sleeps==7);balanced();
 reset();regs[0x610630]=0x200a0000;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnTimeout);balanced();
 reset();regs[0x680004]=0x70;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnTimeout);balanced();
 for(UInt32 at:{0x682030U,0x68a030U,0x682088U,0x68a088U,0x6820a0U,0x68a0a0U}){
  reset();regs[at]^=1;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnTimeout);balanced();}
 reset();regs[0x611020]=0x90000080; // ACKed VALID/type NONE, pending is zero
 assert(d.coreException()==0 && acknowledgements==0);
 assert(d.waitCursorArmed(wantUsage,want)==0);balanced();
 reset();regs[0x611854]=1;regs[0x611020]=0x10005080;
 regs[0x611024]=1;regs[0x611028]=0x43;
 assert(d.waitCursorArmed(wantUsage,want)==kIOReturnIOError && acknowledgements==1);
 assert((d.exception==std::vector<UInt32>{0x10005080,1,0x43}));balanced();
 reset(false);faultAfter=3;regs[0x611020]=0x10005080;
 assert(d.waitCursorArmed(wantUsage,want)==kIOReturnIOError && sleeps==3);balanced();
 for(UInt32 at:{0x611854U,0x610630U,0x680000U,0x680004U,0x682030U,0x68a030U,0x68a098U}){
  reset();failRead=at;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnIOError && sleeps==0);balanced();}
 reset();noMap=true;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnNoMemory);balanced();
 reset();mapLength=0x68a0a0;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnNoMemory);balanced();
 reset();d.pci_=nullptr;assert(d.waitCursorArmed(wantUsage,want)==kIOReturnBadArgument && maps==0);
 d.pci_=reinterpret_cast<void*>(1);assert(d.waitCursorArmed(wantUsage,nullptr)==kIOReturnBadArgument);
 reset();NVGspControl show;regs[0x68209c]=regs[0x68a09c]=want[5]&0x7fffffff;
 assert(show.cursorShow(true)==0 && show.cursorVisible_ && show.submits==1);
 assert(show.cursorShow(false)==0 && !show.cursorVisible_ && show.submits==2);balanced();
 reset();NVGspControl rejected;rejected.submitMode=1;
 regs[0x68209c]=regs[0x68a09c]=want[5]&0x7fffffff;
 assert(rejected.cursorShow(true)==kIOReturnTimeout && sleeps==200 && rejected.submits==2);
 assert(!rejected.cursorVisible_ && !rejected.cursorControl_);
 assert(rejected.props["NVGspControl-cursor-failure-disable-result"]==0);
 assert(rejected.cursorShow(true)==kIOReturnNotReady && rejected.submits==2);balanced();
 reset();NVGspControl failed;failed.submitMode=2;
 assert(failed.cursorShow(true)==kIOReturnIOError && failed.submits==2 && sleeps==0);
 assert(failed.props["NVGspControl-cursor-failure-disable-result"]==kIOReturnIOError);balanced();
 reset();NVGspControl unready;unready.cursorReady_=false;
 assert(unready.cursorShow(true)==kIOReturnNotReady && unready.submits==0);
 reset();NVGspControl unread;failRead=0x68a030;
 assert(unread.cursorShow(true)==kIOReturnIOError && unread.submits==0);
 std::puts("actual cursor ARM: ASSY/fetch versus ARM, bounded waits, pending exceptions, map/read failures and show/cleanup PASS");
}
'''

def method(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT/'drivers/NVGspControl/NVGspControl.cpp')
    source = ap.parse_args().source.read_text()
    methods = method(source, 'UInt32 NVGspControl::coreException()', '// 0.84.0: hardware cursor')
    methods += method(source, 'IOReturn NVGspControl::cursorShow(', '// 0.138.0: D2 bring-up')
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        (p/'check.cpp').write_text(PREFIX+methods+SUFFIX)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(p/'check.cpp'), '-o', str(p/'check')], check=True)
        subprocess.run([str(p/'check')], check=True)

if __name__ == '__main__':
    main()
