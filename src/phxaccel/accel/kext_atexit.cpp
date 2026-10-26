// Newer upstream clang (seen with 23.1) lowers the static destructors (the OSMetaClass objects) to a __cxa_atexit
// registration on Darwin even under -mkernel/-fapple-kext, and the kernel exports no __cxa_atexit: such a kext would
// not link at load. Apple's clang puts them in __mod_term_func instead. Keep that behaviour (as PhxBringup does): record
// what gets registered and run it, newest first, from our own __mod_term_func entry when the kext is unloaded.
// With Apple's clang nothing registers and the entry runs nothing.
extern "C" {
static void (*gAtexitFn[64])(void *);
static void *gAtexitArg[64];
static unsigned gAtexitN;
__attribute__((visibility("hidden"))) int __cxa_atexit(void (*fn)(void *), void *arg, void *) {
    if (gAtexitN >= 64)
        return -1;
    gAtexitFn[gAtexitN] = fn;
    gAtexitArg[gAtexitN++] = arg;
    return 0;
}
static void phxAccelRunAtexit() {
    while (gAtexitN) {
        gAtexitN--;
        gAtexitFn[gAtexitN](gAtexitArg[gAtexitN]);
    }
}
__attribute__((used, section("__DATA,__mod_term_func,mod_term_funcs"))) static void (*const gPhxAccelTerm)() = phxAccelRunAtexit;
}
