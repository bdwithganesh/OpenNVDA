#include <mach/mach_types.h>
#include <mach/kmod.h>
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
// 1.0.0: new bundle id. Unsigned kext approvals are bound to the cdhash, and
// the old id carries a load-history entry macOS can no longer evaluate, so it
// never reached the approval UI.
KMOD_EXPLICIT_DECL(org.local.macosdevicelab.NVDisplayFB10, "1.5.1", _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = nullptr;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = nullptr;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
kern_return_t _start(kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }
kern_return_t _stop(kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }
}
