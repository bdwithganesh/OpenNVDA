#!/usr/bin/env python3
"""Compile actual auto-channel policy; cached NVRAM must not override reset fallback."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "drivers/NVGspControl/NVGspControl.cpp").read_text()
def method(prefix):
    start = source.index(prefix)
    end = source.index("\n}\n", start) + 3
    return source[start:end]
methods = method("static bool postResetPrivateDiagnosticByNvram()") + method("bool NVGspControl::privateChannelGenerationAllowed()") + method("bool NVGspControl::ownChannelAuto()")
assert "if (!privateChannelGenerationAllowed()) { ulk(); return kIOReturnUnsupported; }" in source
assert "resetPrivateDiagnostic_ = gpuResets_ == 0 && postResetPrivateDiagnosticByNvram();" in source
mock = r'''
#include <cassert>
#include <cstring>
#include <iostream>
using UInt32=unsigned;
struct OSObject { virtual ~OSObject()=default; };
struct OSData : OSObject { unsigned getLength() {return 0;} const void *getBytesNoCopy() {return nullptr;} };
struct OSString : OSObject { const char *value="1"; bool isEqualTo(const char *s) {return !strcmp(s,value);} };
const char *diagnosticValue=nullptr;
#define OSDynamicCast(T,o) dynamic_cast<T *>(o)
#define OSSafeReleaseNULL(o) do {o=nullptr;} while(0)
const char *gIODTPlane="mock";
struct IORegistryEntry {
 static IORegistryEntry *fromPath(const char *,const char *) {static IORegistryEntry r;return &r;}
 OSObject *copyProperty(const char *key) {
  static OSString on, diag;
  if (!strcmp(key,"nvgsp-postreset-private")) {
   if (!diagnosticValue) return nullptr;
   diag.value=diagnosticValue;return &diag;
  }
  return &on;
 }
 void release() {}
};
struct NVGspControl {
 UInt32 gpuResets_=0; bool resetPrivateDiagnostic_=false;
 bool privateChannelGenerationAllowed() const;
 void setProperty(const char *, bool) {}
 bool ownChannelAuto();
};
'''
main = r'''
int main() {
 assert(!postResetPrivateDiagnosticByNvram());
 diagnosticValue="0";assert(!postResetPrivateDiagnosticByNvram());
 diagnosticValue="yes";assert(!postResetPrivateDiagnosticByNvram());
 diagnosticValue="1";assert(postResetPrivateDiagnosticByNvram());
 NVGspControl d;
 assert(d.ownChannelAuto()); // caches NVRAM1 before first reset
 d.gpuResets_=1;
 assert(!d.ownChannelAuto()); // cached enable must not reopen damaged contexts
 d.gpuResets_=2;
 assert(!d.ownChannelAuto());
 NVGspControl rebooted;
 assert(rebooted.ownChannelAuto()); // fresh hardware boot can use private channels
 NVGspControl diag;diag.resetPrivateDiagnostic_=true;diag.gpuResets_=1;
 assert(diag.ownChannelAuto() && diag.privateChannelGenerationAllowed());
 diag.gpuResets_=2;
 assert(!diag.ownChannelAuto() && !diag.privateChannelGenerationAllowed());
 diag.gpuResets_=1;diag.resetPrivateDiagnostic_=false;
 assert(!diag.ownChannelAuto()); // S3/retry clears diagnostic; NVRAM1 does not bypass the member gate
 std::cout << "cached enable, default fallback, bounded first-reset diagnostic, second reset/S3 cancellation, fresh boot: PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix="nvgsp-reset-policy-") as tmp:
 cpp, exe = Path(tmp)/"check.cpp", Path(tmp)/"check"
 cpp.write_text(mock + methods + main)
 subprocess.run(["clang++", "-std=c++17", "-fsanitize=undefined", str(cpp), "-o", str(exe)], check=True)
 subprocess.run([str(exe)], timeout=10, check=True)
