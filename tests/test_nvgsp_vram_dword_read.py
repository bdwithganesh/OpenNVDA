#!/usr/bin/env python3
"""Run actual selector11 and diagnostic reader across DWORD/QWORD boundaries."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UInt32=uint32_t;using UInt64=uint64_t;using IOReturn=int;using io_connect_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnIOError=2;
struct PraminPteResult{};static unsigned locked=0,reads=0;static bool fail=false,oldKernel=false;
static uint32_t word(uint64_t address){return 0xabc00000+uint32_t(address/4);}
bool praminPteAccess(void*,uint64_t at,uint64_t *v,bool,bool,PraminPteResult*){
 assert(locked==1);++reads;if((at&7) || fail)return false;
 *v=uint64_t(word(at))|(uint64_t(word(at+4))<<32);return true;
}
bool praminWriteWords(void*,uint64_t,const uint32_t*,unsigned){assert(locked==1);return !fail;}
struct NVGspControl {
 void *lock_=this,*pci_=this;
 void lk(unsigned){assert(!locked);++locked;}void ulk(){assert(locked==1);--locked;}
 IOReturn vramAccess(UInt64,UInt32*,UInt32,bool);
};
static NVGspControl driver;
int IOConnectCallMethod(int,unsigned selector,const uint64_t *in,unsigned n,
 const void*,size_t,void*,void*,void *out,size_t *bytes){
 assert(selector==11 && n==3 && !in[2] && in[1]<=1024 && *bytes==in[1]*4);
 if(oldKernel && (in[0]&7))return kIOReturnIOError;
 return driver.vramAccess(in[0],static_cast<uint32_t*>(out),unsigned(in[1]),false);
}
'''
SUFFIX = r'''
int main(){
 for(uint64_t address:{0x58ULL,0x8cULL,0xccULL,0xfffcULL,0x10000ULL}){
  for(unsigned count:{1U,2U,3U,13U,1024U}){
   uint32_t out[1026];for(auto &v:out)v=0xdeadbeef;
   assert(driver.vramAccess(address,out+1,count,false)==0 && !locked);
   assert(out[0]==0xdeadbeef && out[count+1]==0xdeadbeef);
   for(unsigned i=0;i<count;++i)assert(out[i+1]==word(address+i*4));
  }
  for(bool old:{false,true})for(unsigned count:{1U,3U,13U,1024U,1025U,2058U}){
   oldKernel=old;uint32_t out[2060];for(auto &v:out)v=0xdeadbeef;
   assert(vread(0,address,out+1,count) && !locked);
   assert(out[0]==0xdeadbeef && out[count+1]==0xdeadbeef);
   for(unsigned i=0;i<count;++i)assert(out[i+1]==word(address+i*4));
  }
 }
 uint32_t out[3]={1,2,3};fail=true;
 assert(driver.vramAccess(4,out,1,false)==kIOReturnIOError && !locked && out[0]==1);
 assert(!vread(0,4,out,1) && !locked);fail=false;
 const auto before=reads;
 assert(driver.vramAccess(2,out,1,false)==kIOReturnBadArgument && reads==before);
 assert(!vread(0,2,out,1) && reads==before);
 assert(!vread(0,4,nullptr,1) && !vread(0,4,out,0));
 puts("actual selector11 and old-kernel diagnostic reads: DWORD alignment, chunk edges, guards and failure cleanup PASS");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'drivers/NVGspControl/NVGspControl.cpp')
    source = parser.parse_args().source.read_text()
    method = source[source.index('IOReturn NVGspControl::vramAccess('):
                    source.index('IOReturn NVGspControl::pokeBar0(')]
    header = (ROOT/'tools/display/nvdisplay_admin.hpp').read_text()
    reader = header[header.index('static inline bool vread('):header.index('static inline bool vwrite(')]
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path/'check.cpp').write_text(PREFIX+method+reader+SUFFIX)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(path/'check.cpp'), '-o', str(path/'check')], check=True)
        subprocess.run([str(path/'check')], check=True)


if __name__ == '__main__':
    main()
