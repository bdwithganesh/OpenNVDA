#!/usr/bin/env python3
"""Exercise the real ordered-stamp routine with closed/reused channel slots."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MOCK = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
using UInt32=uint32_t; using UInt64=uint64_t; using UInt16=uint16_t; using SInt32=int32_t;
class NVGspControl {
public:
    static constexpr UInt32 kMaxClientChannels=2, kStampQ=8, kStampCtxOff=0;
    struct ClientChannel {UInt32 state=0,memHandle=0,pbOff=0,seq=0,serial=0;};
    struct PendingStamp {UInt32 value=0,seq=0,offset=0,serial=0; UInt16 slot=0,pad=0;};
    struct Ring {UInt32 slot; UInt32 *pbOff,*seq;};
    ClientChannel cchan_[2]{}; PendingStamp stampQ_[8]{};
    UInt32 stampQHead_=0,stampQTail_=0,stampWritten_=0,stampDeliverMask_=0;
    bool grPersistent_=true; UInt64 ctxBackingOffset_=1;
    UInt32 semaphore[3]{},written=0;
    bool grRing(Ring *r) {*r={2,nullptr,nullptr}; return true;}
    bool clientRingLocked(ClientChannel *c,Ring *r) {
        if (c->state!=2) return false;
        UInt32 slot=c==&cchan_[0]?0:c==&cchan_[1]?1:0;
        *r={slot,&c->pbOff,&c->seq}; return true;
    }
    bool readRingSem(const Ring &r,UInt32 *v) {*v=semaphore[r.slot]; return true;}
    bool ringWrite(UInt64,const UInt32 *v,UInt32) {written=*v; return true;}
    bool stampAdvanceLocked();
};
int main() {
    NVGspControl d;
    d.cchan_[0]={2,1,0,10,100}; d.cchan_[1]={2,1,0,50,200};
    d.stampQ_[0]={1,10,0,100,0,0}; d.stampQ_[1]={2,50,0,200,1,0};
    d.stampQTail_=2; d.semaphore[0]=9; d.semaphore[1]=50;
    assert(!d.stampAdvanceLocked()); // later completion must wait for first
    // B drains/closes while A is pending, then C opens in B's slot with seq 1.
    d.cchan_[1]={2,1,0,1,201}; d.semaphore[1]=1; d.semaphore[0]=10;
    d.stampAdvanceLocked();
    if (d.stampQHead_!=2 || d.written!=2) {
        std::cerr<<"closed slot reused: retired "<<d.stampQHead_<<" of 2 stamps\n";
        return 1;
    }
    // New channel C's own pending fence must still block until it lands.
    d.stampQ_[2]={3,2,0,201,1,0}; d.stampQTail_=3;
    assert(!d.stampAdvanceLocked() && d.written==2);
    d.semaphore[1]=2; assert(d.stampAdvanceLocked() && d.written==3);
    // Ring sequence wrap uses circular comparison, and shared-ring stamps wait.
    d.stampQ_[3]={4,0xffffffff,0,201,1,0}; d.stampQTail_=4;
    d.semaphore[1]=0; assert(d.stampAdvanceLocked() && d.written==4);
    d.stampQ_[4]={5,5,0,0,0xffff,0}; d.stampQTail_=5;
    d.semaphore[2]=4; assert(!d.stampAdvanceLocked());
    d.semaphore[2]=5; assert(d.stampAdvanceLocked() && d.written==5);
    assert(d.stampDeliverMask_==1);
    std::cout<<"ordered stamps: closed/reused slots, pending fences, wrap, shared ring PASS\n";
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path,
                    default=ROOT / "drivers/NVGspControl/NVGspControl.cpp")
    source = ap.parse_args().source.read_text()
    start = source.index("bool NVGspControl::stampAdvanceLocked()")
    end = source.index("\nbool NVGspControl::clientRingLocked", start)
    with tempfile.TemporaryDirectory(prefix="nvgsp-stamp-reuse-") as tmp:
        cpp, exe = Path(tmp) / "check.cpp", Path(tmp) / "check"
        cpp.write_text(MOCK + source[start:end])
        subprocess.run(["clang++", "-std=c++17", "-fsanitize=undefined",
                        str(cpp), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], timeout=10).returncode


if __name__ == "__main__":
    raise SystemExit(main())
