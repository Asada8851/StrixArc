//
//  kmod_info.c — XNU kmod glue for RDNA3Driver.kext
//  Exports the mandatory _kmod_info symbol required by OpenCore PrelinkedInjectKext.
//
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

kern_return_t _start(kmod_info_t *ki, void *data) {
    (void)ki;
    (void)data;
    return KERN_SUCCESS;
}

kern_return_t _stop(kmod_info_t *ki, void *data) {
    (void)ki;
    (void)data;
    return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(org.drip.driver.RDNA3Driver, "1.0.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
