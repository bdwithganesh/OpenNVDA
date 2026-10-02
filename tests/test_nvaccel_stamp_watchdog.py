#!/usr/bin/env python3
"""Run actual native callbacks: healthy delay and ordered failed-submission retirement."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--source",type=Path,default=ROOT/"drivers/NVAccelerator/NVAccelerator.cpp")
    args=ap.parse_args()
    s=args.source.read_text()
    def method(name):
        start=s.index("void NVAccelerator::"+name+"(")
        end=s.index("\n}\n",start)+3
        return s[start:end]
    extra=""
    for name in ("beginNativeStamp", "recordNativeStamp", "retireCanceledStamps"):
        if "void NVAccelerator::"+name+"(" in s:
            extra+=method(name)
        else:
            extra+={"beginNativeStamp":"void NVAccelerator::beginNativeStamp(UInt32) {}\n",
                    "recordNativeStamp":"void NVAccelerator::recordNativeStamp(UInt32,UInt32,bool) {}\n",
                    "retireCanceledStamps":"void NVAccelerator::retireCanceledStamps() {}\n"}[name]
    mock=r'''
    #include <cassert>
    #include <cstdint>
    #include <cstring>
    #include <iostream>
    using UInt32=uint32_t; using UInt64=uint64_t; using SInt32=int32_t;
    using thread_call_param_t=void*;
    const int kMicrosecondScale=1;
    UInt64 clockNow=4000000001ULL;
    void clock_get_uptime(UInt64 *v) {*v=clockNow;}
    void absolutetime_to_nanoseconds(UInt64 v,UInt64 *out) {*out=v;}
    void clock_interval_to_deadline(int,int,UInt64 *out) {*out=0;}
    void thread_call_enter_delayed(void*,UInt64) {}
    #define NVALOG(...) do {} while(0)
    struct IOLock {bool held=false;};
    void IOLockLock(IOLock *l) {assert(!l->held);l->held=true;}
    void IOLockUnlock(IOLock *l) {assert(l->held);l->held=false;}
    struct OSObject {virtual ~OSObject()=default;};
    struct OSBoolean:OSObject {bool value=false; bool isTrue(){return value;}};
    #define OSDynamicCast(T,o) dynamic_cast<T*>(o)
    #define OSSafeReleaseNULL(o) do {o=nullptr;} while(0)
    struct IOAccelEventMachine2 {void *getStamp(unsigned i) {return reinterpret_cast<void*>(i?0:10);}};
    struct Map {UInt32 words[2]={}; uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(words);}};
    struct Gsp {
     Map *map; bool ready=true,reset=false,sleep=false,missing=false,ringDone=false; unsigned polls=0;
     OSBoolean prop;
     void callPlatformFunction(const char*,bool,void*,void*,void*,void*) {++polls;if(ringDone)map->words[0]=10;}
     OSObject *copyProperty(const char *key) {
      if(missing)return nullptr;
      prop.value=!strcmp(key,"NVGspControl-reset-busy")?reset:!strcmp(key,"NVGspControl-sleeping")?sleep:ready;
      return &prop;
     }
    };
    struct NVAccelerator {
     bool pollStop_=false; UInt32 pollArmed_=1,pollIdle_=0,forcedCompletions_=0;
     UInt32 stampSignaled_[2]={},stampSubmitted_[2]={10,0}; UInt64 stampProgressAbs_=1;
     UInt32 stampAccepted_[2]={},stampCanceled_[2]={},stampCancelAfter_[2]={},canceledCompletions_=0;
     bool stampCancelPending_[2]={},stampSubmitting_[2]={}; IOLock lock,cancelLock;
     IOLock *submitLock_=&lock,*stampCancelLock_=&cancelLock;
     void beginNativeStamp(UInt32);
     void recordNativeStamp(UInt32,UInt32,bool);
     void retireCanceledStamps();
     void submit(unsigned i,UInt32 value,bool accepted) {
      IOLockLock(submitLock_);recordNativeStamp(i,value,accepted);IOLockUnlock(submitLock_);
     }
     Map map; Map *stampMap_=&map; Gsp gsp{&map}; Gsp *gsp_=&gsp;
     void *pollCall_=this; IOAccelEventMachine2 em;
     IOAccelEventMachine2 *eventMachine(){return &em;}
     void checkStamps(){stampSignaled_[0]=map.words[0];stampSignaled_[1]=map.words[1];}
     void kickStampPoll(){}
     static void pollCallout(thread_call_param_t,thread_call_param_t);
    };
    '''
    main=r'''
    int main(){
     NVAccelerator healthy;
     NVAccelerator::pollCallout(&healthy,nullptr);
     if(healthy.map.words[0]||healthy.forcedCompletions_) {
      std::cout<<"FAIL: healthy delayed work was artificially completed\n";return 11;
     }
     assert(healthy.gsp.polls==1); // ordered GSP queue actively polled
     NVAccelerator landed; landed.gsp.ringDone=true;
     NVAccelerator::pollCallout(&landed,nullptr);
     assert(landed.stampSignaled_[0]==10 && landed.forcedCompletions_==0);
     NVAccelerator reset;reset.gsp.reset=true;
     NVAccelerator::pollCallout(&reset,nullptr);
     assert(reset.map.words[0]==10 && reset.forcedCompletions_==1);
     NVAccelerator sleeping;sleeping.gsp.sleep=true;
     NVAccelerator::pollCallout(&sleeping,nullptr);
     assert(sleeping.forcedCompletions_==1);
     NVAccelerator offline;offline.gsp.ready=false;
     NVAccelerator::pollCallout(&offline,nullptr);
     assert(offline.forcedCompletions_==1);
     NVAccelerator unknown;unknown.gsp.missing=true;
     NVAccelerator::pollCallout(&unknown,nullptr);
     assert(!unknown.forcedCompletions_ && unknown.map.words[0]==0);
     NVAccelerator stopped;stopped.pollStop_=true;
     NVAccelerator::pollCallout(&stopped,nullptr);
     assert(stopped.pollArmed_==0 && stopped.gsp.polls==0);
     NVAccelerator canceled;canceled.submit(0,10,true);canceled.submit(0,11,false);
     NVAccelerator::pollCallout(&canceled,nullptr);
     assert(canceled.map.words[0]==0); // accepted work still running
     canceled.map.words[0]=10;
     NVAccelerator::pollCallout(&canceled,nullptr);
     if(canceled.map.words[0]!=11 || canceled.canceledCompletions_!=1) {
      std::cout<<"FAIL: last rejected stamp did not retire after its real prerequisite\n";return 12;
     }
     NVAccelerator fifo;fifo.submit(0,10,true);fifo.submit(0,11,false);fifo.map.words[0]=10;
     IOLockLock(fifo.submitLock_); // family waits for a reused FIFO event while holding submit lock
     NVAccelerator::pollCallout(&fifo,nullptr);
     IOLockUnlock(fifo.submitLock_);
     assert(fifo.map.words[0]==11);
     NVAccelerator inFlight;inFlight.submit(0,10,true);inFlight.submit(0,11,false);inFlight.map.words[0]=10;
     inFlight.beginNativeStamp(0);NVAccelerator::pollCallout(&inFlight,nullptr);
     assert(inFlight.map.words[0]==10); // GSP may be kicking a newer value
     inFlight.submit(0,12,false);NVAccelerator::pollCallout(&inFlight,nullptr);
     assert(inFlight.map.words[0]==12);
     NVAccelerator consecutive;consecutive.submit(0,10,true);
     consecutive.submit(0,11,false);consecutive.submit(0,12,false);
     consecutive.map.words[0]=9;NVAccelerator::pollCallout(&consecutive,nullptr);
     assert(consecutive.map.words[0]==9);
     consecutive.map.words[0]=10;NVAccelerator::pollCallout(&consecutive,nullptr);
     assert(consecutive.map.words[0]==12 && consecutive.canceledCompletions_==1);
     NVAccelerator later;later.submit(0,10,true);later.submit(0,11,false);later.submit(0,12,true);
     later.map.words[0]=10;NVAccelerator::pollCallout(&later,nullptr);
     assert(later.map.words[0]==10); // do not race the newer GPU write with cancellation
     later.map.words[0]=12;NVAccelerator::pollCallout(&later,nullptr);
     assert(later.map.words[0]==12 && !later.stampCancelPending_[0] && !later.canceledCompletions_);
     NVAccelerator separated;separated.submit(0,10,true);separated.submit(0,11,false);
     separated.submit(0,12,true);separated.submit(0,13,false);
     separated.map.words[0]=10;NVAccelerator::pollCallout(&separated,nullptr);
     assert(separated.map.words[0]==10);
     separated.map.words[0]=12;NVAccelerator::pollCallout(&separated,nullptr);
     assert(separated.map.words[0]==13);
     NVAccelerator independent;independent.submit(0,10,true);independent.submit(0,11,false);
     independent.submit(1,1,false);NVAccelerator::pollCallout(&independent,nullptr);
     assert(independent.map.words[0]==0 && independent.map.words[1]==1);
     NVAccelerator wrapped;wrapped.stampSubmitted_[0]=wrapped.stampAccepted_[0]=0xfffffffe;
     wrapped.submit(0,0xffffffff,false);wrapped.submit(0,0,false);
     wrapped.map.words[0]=0xfffffffd;NVAccelerator::pollCallout(&wrapped,nullptr);
     assert(wrapped.map.words[0]==0xfffffffd);
     wrapped.map.words[0]=0xfffffffe;NVAccelerator::pollCallout(&wrapped,nullptr);
     assert(wrapped.map.words[0]==0 && wrapped.canceledCompletions_==1);
     std::cout<<"healthy delay, actual fence, reset/sleep/offline, missing state, stop; ordered/consecutive/later/separated/wrapped cancellations, independent engines, FIFO-lock and in-flight kick: PASS\n";
    }
    '''
    with tempfile.TemporaryDirectory(prefix="nvaccel-stamp-poll-") as tmp:
     cpp,exe=Path(tmp)/"check.cpp",Path(tmp)/"check"
     cpp.write_text(mock+extra+method("pollCallout")+main)
     subprocess.run(["clang++","-std=c++17","-fsanitize=undefined",str(cpp),"-o",str(exe)],check=True)
     subprocess.run([str(exe)],timeout=10,check=True)

if __name__ == '__main__':
    main()
