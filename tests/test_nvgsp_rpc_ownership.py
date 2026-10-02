#!/usr/bin/env python3
"""Compile the real RPC methods with a deterministic two-caller scheduler.

The status poller delivers A's reply while A sleeps, then B tries to take the
RPC slot before A copies the reply. The old implementation returns B's reply
to A. No hardware or macOS kernel SDK required.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def extract(src, signature):
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (src[end] == "{") - (src[end] == "}")
        end += 1
    return src[start:end]


MOCK = r'''
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
using UInt32 = uint32_t;
using UInt8 = uint8_t;
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1,
                  kIOReturnBusy=2, kIOReturnIOError=3, kIOReturnTimeout=4;
namespace nvgsp { struct GspQueueElementHeader {char bytes[48];};
                  struct RpcMessageHeader {char bytes[32];}; }
std::mutex schedule;
std::condition_variable changed;
bool aSleeping=false, bSleeping=false, releaseA=false, aDone=false, bDone=false;
thread_local int role=0;
void IOSleep(int) {
    std::unique_lock<std::mutex> l(schedule);
    if (role==1) {
        aSleeping=true; changed.notify_all();
        changed.wait(l, [] {return releaseA;});
    } else if (role==2) {
        bSleeping=true; changed.notify_all();
        changed.wait(l, [] {return aDone;});
    }
}
class NVGspControl {
public:
    std::mutex mutex;
    void *lock_ = this;
    UInt32 postInitPhase_=33;
    bool sleeping_=false, pingOutstanding_=false, userRpcOutstanding_=false,
         userRpcActive_=false, failEnqueue=false, noReply=false, largeReplyOk_=false;
    UInt32 userRpcFunction_=0, userRpcReplyBytes_=0, userRpcResult_=0, userRpcs_=0,
           userRpcSeq_=0, userRpcSeqCounter_=0, pending=0;
    UInt8 userRpcReply_[4096]{};
    struct Init {
        NVGspControl *d;
        bool enqueueRpc(UInt32, const UInt8 *p, UInt32, UInt32 = 0) {
            if (d->failEnqueue) return false;
            d->pending=*p;
            return true;
        }
    } init_{this};
    void lk(int) {mutex.lock();}
    void ulk() {mutex.unlock();}
    void setProperty(const char *, UInt32, int) {}
    void complete() {
        userRpcReply_[0]=static_cast<UInt8>(pending);
        userRpcReplyBytes_=1; userRpcResult_=pending+100;
        userRpcOutstanding_=false;
    }
    void pollStatusLocked() {
        if (!noReply && pending==2) complete();
    }
    bool waitRpcSlotLocked(UInt32);
    IOReturn userRpc(UInt32, const UInt8 *, UInt32, UInt8 *, UInt32 *, UInt32 *);
    IOReturn userRpcLarge(UInt32, const UInt8 *, UInt32, UInt32 *);
};
'''

MAIN = r'''
bool race(bool largeA, bool largeB) {
    aSleeping=bSleeping=releaseA=aDone=bDone=false;
    NVGspControl d;
    UInt8 a=0, b=0;
    IOReturn ar=9, br=9;
    UInt32 aResult=0, bResult=0;
    std::thread first([&] {
        role=1; UInt8 request=1; UInt32 size=1, result=99;
        ar=largeA ? d.userRpcLarge(103,&request,1,&result)
                  : d.userRpc(103, &request, 1, &a, &size, &result);
        aResult=result;
        std::lock_guard<std::mutex> l(schedule);
        aDone=true; changed.notify_all();
    });
    { std::unique_lock<std::mutex> l(schedule);
      changed.wait(l, [] {return aSleeping;}); }
    d.lk(0); d.complete(); d.ulk(); // daemon delivers A while A is asleep
    std::thread second([&] {
        role=2; UInt8 request=2; UInt32 size=1, result=99;
        br=largeB ? d.userRpcLarge(103,&request,1,&result)
                  : d.userRpc(103, &request, 1, &b, &size, &result);
        bResult=result;
        std::lock_guard<std::mutex> l(schedule);
        bDone=true; changed.notify_all();
    });
    { std::unique_lock<std::mutex> l(schedule);
      changed.wait(l, [] {return bSleeping || bDone;});
      releaseA=true; changed.notify_all(); }
    first.join(); second.join();
    if (ar || br || (!largeA && a!=1) || (!largeB && b!=2) || aResult!=101 || bResult!=102) {
        std::cerr << "RPC reply stolen: A=" << unsigned(a)
                  << " B=" << unsigned(b) << " results=" << aResult << "," << bResult << "\n";
        return false;
    }
    assert(!d.userRpcActive_ && !d.userRpcOutstanding_);
    return true;
}
int main() {
    if (!race(false,false) || !race(true,false) || !race(false,true)) return 1;
    NVGspControl d;
    role=0; UInt8 request=2, output=0; UInt32 size=1, result=99;
    d.failEnqueue=true;
    assert(d.userRpc(103,&request,1,&output,&size,&result)==kIOReturnIOError);
    assert(!d.userRpcActive_ && !d.userRpcOutstanding_);
    d.failEnqueue=false; d.noReply=true; size=1;
    assert(d.userRpc(103,&request,1,&output,&size,&result)==kIOReturnTimeout);
    assert(!d.userRpcActive_ && !d.userRpcOutstanding_ && size==0);
    d.noReply=false; size=1;
    assert(d.userRpc(103,&request,1,&output,&size,&result)==kIOReturnSuccess);
    assert(output==2 && !d.userRpcActive_);
    std::cout << "RPC normal/large caller ownership, enqueue failure, timeout, reuse: PASS\n";
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path,
                    default=ROOT / "drivers/NVGspControl/NVGspControl.cpp")
    args = ap.parse_args()
    source = args.source.read_text()
    code = MOCK + extract(source, "bool NVGspControl::waitRpcSlotLocked(")
    code += extract(source, "IOReturn NVGspControl::userRpc(")
    code += extract(source, "IOReturn NVGspControl::userRpcLarge(") + MAIN
    with tempfile.TemporaryDirectory(prefix="nvgsp-rpc-owner-") as tmp:
        cpp = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check"
        cpp.write_text(code)
        subprocess.run(["clang++", "-std=c++17", "-pthread", "-fsanitize=undefined",
                        str(cpp), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], timeout=15).returncode


if __name__ == "__main__":
    raise SystemExit(main())
