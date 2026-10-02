#!/usr/bin/env python3
"""Execute the actual paired-probe cursor PIO setup and snapshot guard."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using io_connect_t=int;using io_service_t=int;
#define CFSTR(x) x
struct Snapshot{uint32_t armed[7],usage[2],pending,error;};
struct LutState{Snapshot display;};
static unsigned setups,reads;
static uint64_t pushbufStatus,allocStatus;
static bool transportFailed,readFailed,idle,mapFailed;
static Snapshot live;
int IOConnectCallScalarMethod(int,unsigned selector,const uint64_t *in,unsigned n,void*,void*){
 ++setups;assert(selector==35 && n==1 && *in==0);return transportFailed?1:0;}
bool property_u64(int,const char* key,uint64_t *out){
 *out=!strcmp(key,"NVGspControl-cursor-pushbuf-status")?pushbufStatus:allocStatus;return true;}
bool read_state(int,LutState* out){++reads;out->display=live;return !readFailed;}
bool armed_matches(const Snapshot*,uint32_t,const uint32_t*){return idle;}
bool vread(int,uint64_t,uint32_t* out,unsigned n){memset(out,0,n*4);return !mapFailed;}
static int run(){
 const bool cursorOutput=true;const io_connect_t c=0;const io_service_t service=0;
 uint64_t inst=0x10000;LutState before={};uint32_t originalHash[2048],originalObjects[64];
SETUP_SOURCE
 return 0;
}
static void reset(){setups=reads=0;pushbufStatus=allocStatus=0;
 transportFailed=readFailed=mapFailed=false;idle=true;live={};live.usage[0]=live.usage[1]=0x1110;}
int main(){
 reset();assert(run()==0 && setups==1 && reads==1);
 reset();transportFailed=true;assert(run()==1 && setups==1 && reads==0);
 reset();pushbufStatus=0x56;assert(run()==1 && reads==0);
 reset();allocStatus=0x56;assert(run()==1 && reads==0);
 reset();readFailed=true;assert(run()==1);
 reset();idle=false;assert(run()==1);
 reset();live.pending=1;assert(run()==1);
 reset();live.error=1;assert(run()==1);
 reset();live.armed[0]=0xc0d0d002;assert(run()==1);
 reset();live.armed[1]=0xc0d0d002;assert(run()==1);
 reset();live.armed[5]=0x800001cf;assert(run()==1);
 reset();live.usage[1]=0x1112;assert(run()==1);
 reset();mapFailed=true;assert(run()==1);
 puts("actual cursor PIO setup: status/transport/idle/disabled-state/backup failures refuse before cursor output trial PASS");
}
'''
def main():
    s=(ROOT/'tools/display/nvdisplay_olut_probe.cpp').read_text()
    a=s.index('    if(cursorOutput){');b=s.index('    if (originalHash[slot*2]',a)
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'check.cpp').write_text(PREFIX.replace('SETUP_SOURCE',s[a:b]))
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)
if __name__=='__main__':main()
