#!/usr/bin/env python3
"""Execute actual diagnostic CLI dispatch and half-gain OLUT construction."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
using io_service_t=int;using io_connect_t=int;
constexpr int kIOMainPortDefault=0;
#define CFSTR(x) x
struct LutState{};
const char* IOServiceMatching(const char* s){return s;}
int IOServiceGetMatchingService(int,const char*){return 1;}
int mach_task_self(){return 0;}
int IOServiceOpen(int,int,int,int* c){*c=1;return 0;}
void IOServiceClose(int){}
void IOObjectRelease(int){}
bool read_state(int,LutState*){return false;}
bool property_u64(int,const char*,uint64_t*){return false;}
constexpr uint64_t vramBytes=0x400000000ULL;
bool vread(int,uint64_t,uint32_t*,unsigned){return false;}
int peek(int,uint32_t,unsigned,uint32_t*){return 1;}
void dump_state(const char*,const LutState&){}
static unsigned trials;
static std::array<bool,6> selected;
int trial(int,int,bool half,bool ocsc,bool paired,bool cursor,bool output,bool cursorOutput){
 ++trials;selected={half,ocsc,paired,cursor,output,cursorOutput};return 0;}
'''
SUFFIX=r'''
int main(){
 char program[]="probe";
 for(const char* mode:{"--paired-output-identity","--paired-output-half","--paired-output-cursor","--paired-identity","--paired-cursor","--half-gain"}){
  char* argv[]={program,const_cast<char*>(mode)};
  assert(probe_main(2,argv)==0);
  const bool cursorOutput=!strcmp(mode,"--paired-output-cursor");
  const bool output=!strcmp(mode,"--paired-output-identity") || !strcmp(mode,"--paired-output-half") || cursorOutput;
  const bool paired=output || !strcmp(mode,"--paired-identity") || !strcmp(mode,"--paired-cursor");
  assert(selected[0]==(!strcmp(mode,"--half-gain") || !strcmp(mode,"--paired-output-half")));
  assert(selected[1]==paired && selected[2]==paired && selected[3]==(!strcmp(mode,"--paired-cursor") || cursorOutput) && selected[4]==output && selected[5]==cursorOutput);
 }
 char invalid[]="--paired-output-random";char* argv[]={program,invalid};
 assert(probe_main(2,argv)==2 && trials==6);
 for(bool halfGain:{false,true}){
TABLE_SOURCE
  for(unsigned i=0;i<8;++i)assert(!table[i]);
  for(unsigned i=0;i<1025;++i){const unsigned step=i>1023?1023:i;
   const uint32_t expected=(step<<6)/(halfGain?2:1);
   assert(table[8+i*2]==(expected|(expected<<16)) && table[9+i*2]==expected);}
 }
 puts("actual output diagnostic: guarded CLI flags and complete fixed/half LUT payload including endpoint PASS");
}
'''

def main():
    cpp=(ROOT/'tools/display/nvdisplay_olut_probe.cpp').read_text()
    entry=cpp[cpp.index('int main('):].replace('int main(', 'int probe_main(',1)
    table=cpp[cpp.index('    uint32_t table[nvgsp::kIdentityLutWords];'):cpp.index('    for (unsigned i = 0; i < nvgsp::kIdentityLutWords; i += 1024)')]
    lut=(ROOT/'drivers/NVGspCore/NVGspDisplayLut.hpp').read_text().replace('#pragma once','')
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'check.cpp').write_text(PREFIX+lut+entry+SUFFIX.replace('TABLE_SOURCE',table))
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)

if __name__=='__main__':
    main()
