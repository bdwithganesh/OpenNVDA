#!/usr/bin/env python3
"""Run the actual production PROMOTE_CTX block against dirty recycled VRAM.

RM's bInitialize contract requires the client to clear those buffers first.
The fixture also checks that a failed clear prevents promotion, keeps global
non-initialized buffers intact, and frees the request allocation.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;
namespace nvgsp { constexpr UInt32 kEngineTypeGraphics=1; }
constexpr UInt64 kBase=0x8200000, kCtxVa=0x104000000ULL;
constexpr UInt32 kPromoteBytes=560;
std::vector<UInt8> vram(0x2a00000, 0xa5);
unsigned zeroCalls=0, failAt=0, freeCalls=0, enqueues=0;
bool clearedProperty=false, enqueueOk=true;
void IOFree(void *p, size_t) {++freeCalls; std::free(p);}
bool praminZeroRange(void *, UInt64 address, UInt64 bytes) {
    if (++zeroCalls==failAt) return false;
    if (address<kBase || address-kBase+bytes>vram.size()) throw std::runtime_error("bad zero bounds");
    std::fill(vram.begin()+address-kBase,vram.begin()+address-kBase+bytes,0);
    return true;
}
struct Init {
    bool enqueueRpc(UInt32 function, const UInt8 *params, UInt32 bytes) {
        ++enqueues;
        if (function!=76 || bytes!=584) throw std::runtime_error("bad RPC");
        const UInt8 *pp=params+24;
        UInt32 count=0; std::memcpy(&count,pp+40,4);
        if (count!=9) throw std::runtime_error("bad entry count");
        unsigned initialized=0;
        for (UInt32 i=0;i<count;++i) {
            const UInt8 *e=pp+48+i*32;
            if (!e[30]) continue;
            ++initialized;
            UInt64 phys=0,size=0; std::memcpy(&phys,e,8); std::memcpy(&size,e+16,8);
            if (phys<kBase || phys-kBase+size>vram.size()) throw std::runtime_error("bad promoted bounds");
            if (std::any_of(vram.begin()+phys-kBase,vram.begin()+phys-kBase+size,
                            [](UInt8 b){return b!=0;})) throw std::runtime_error("RM promoted dirty initialized VRAM");
        }
        if (initialized!=5) throw std::runtime_error("wrong initialized count");
        // Attribute/pagepool/bundle/RTV globals are not cleared by this fix.
        for (auto off: {0u,0x27a0000u,0x27b0000u,0x2800000u})
            if (vram[off]!=0xa5) throw std::runtime_error("non-initialized global was changed");
        return enqueueOk;
    }
} init_;
void setProperty(const char *, bool value) {clearedProperty=value;}
void reset(unsigned failure=0) {
    std::fill(vram.begin(),vram.end(),0xa5); zeroCalls=freeCalls=enqueues=0;
    failAt=failure; clearedProperty=false; enqueueOk=true;
}
void promoteBlock(UInt32 &postInitPhase_, UInt32 &badReason) {
    UInt8 *promote=static_cast<UInt8 *>(std::malloc(584));
    UInt64 ctxBackingOffset_=kBase; void *pci_=reinterpret_cast<void *>(1);
'''
TAIL = r'''
}
int main() {
    try {
        UInt32 phase=217,bad=0; reset(); promoteBlock(phase,bad);
        if (zeroCalls!=5 || enqueues!=1 || freeCalls!=1 || !clearedProperty || phase!=123 || bad)
            throw std::runtime_error("success did not clear-before-promote");
        for (unsigned failure=1;failure<=5;++failure) {
            phase=217;bad=0;reset(failure);promoteBlock(phase,bad);
            if (zeroCalls!=failure || enqueues || freeCalls!=1 || clearedProperty || phase!=217 || bad!=112)
                throw std::runtime_error("failed clear was not contained");
        }
        phase=217;bad=0;reset();enqueueOk=false;promoteBlock(phase,bad);
        if (zeroCalls!=5 || enqueues!=1 || freeCalls!=1 || !clearedProperty || phase!=217 || bad!=112)
            throw std::runtime_error("enqueue failure changed");
        std::cout<<"production GR clear: dirty VRAM, all five ranges, every clear failure, unchanged globals and RPC failure PASS\n";
    } catch (const std::exception &e) {std::cerr<<e.what()<<"\n"; return 12;}
}
'''

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--source',type=Path,default=ROOT/'drivers/NVGspControl/NVGspControl.cpp')
    args=ap.parse_args()
    source=args.source.read_text()
    start=source.index('            if (promote) {',source.index('constexpr UInt32 kPromoteBytes = 560;'))
    brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    block=source[start:end]
    with tempfile.TemporaryDirectory(prefix='nvgsp-context-clear-') as tmp:
        cpp,exe=Path(tmp)/'check.cpp',Path(tmp)/'check'
        cpp.write_text(MOCK+block+TAIL)
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-Wno-unused-variable','-fsanitize=undefined',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],timeout=20,check=True)

if __name__ == '__main__':
    main()
