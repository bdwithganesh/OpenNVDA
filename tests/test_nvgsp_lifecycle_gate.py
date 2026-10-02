#!/usr/bin/env python3
"""Check actual lifecycle waits: mode 2 keeps GR runnable, reset cancels waits."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
using UInt32=uint32_t;
class NVGspControl;
NVGspControl *active;
void IOSleep(int);
class NVGspControl {
public:
    UInt32 gpuResets_=0, sleeps=0, releaseAt=0, resetAt=0;
    bool cchanMutationBusy_=false, cchanLifecycleBusy_=false, locked=true;
    void lk(int) {assert(!locked); locked=true;}
    void ulk() {assert(locked); locked=false;}
    bool waitChannelMutationLocked();
    bool waitChannelLifecycleLocked();
};
void IOSleep(int) {
    assert(!active->locked);
    ++active->sleeps;
    if (active->releaseAt==active->sleeps)
        active->cchanMutationBusy_=active->cchanLifecycleBusy_=false;
    if (active->resetAt==active->sleeps) ++active->gpuResets_;
}
int main() {
    NVGspControl d; active=&d;
    d.cchanLifecycleBusy_=true; d.releaseAt=5;
    assert(d.waitChannelMutationLocked() && d.sleeps==0); // mode2 GR stays runnable
    assert(d.waitChannelLifecycleLocked() && d.sleeps==5 && d.locked);
    d.sleeps=0; d.releaseAt=7; d.cchanMutationBusy_=d.cchanLifecycleBusy_=true;
    assert(d.waitChannelMutationLocked() && d.sleeps==7); // mode1 GR waits
    d.sleeps=0; d.releaseAt=4; d.resetAt=2; d.cchanLifecycleBusy_=true;
    assert(!d.waitChannelLifecycleLocked() && d.locked); // cannot use old generation
    d.sleeps=0; d.releaseAt=d.resetAt=0; d.cchanLifecycleBusy_=true;
    assert(!d.waitChannelLifecycleLocked() && d.sleeps==6000 && d.locked);
    std::cout<<"lifecycle gate: concurrent GR, quiesce, reset cancellation, bounded wait PASS\n";
}
'''

source = (ROOT / "drivers/NVGspControl/NVGspControl.cpp").read_text()
methods = ""
for name in ("waitChannelMutationLocked", "waitChannelLifecycleLocked"):
    start = source.index("bool NVGspControl::" + name + "()")
    end = source.index("\n}\n", start) + 3
    methods += source[start:end]
with tempfile.TemporaryDirectory(prefix="nvgsp-lifecycle-") as tmp:
    cpp, exe = Path(tmp) / "check.cpp", Path(tmp) / "check"
    cpp.write_text(MOCK + methods)
    subprocess.run(["clang++", "-std=c++17", "-fsanitize=undefined",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], timeout=10, check=True)
