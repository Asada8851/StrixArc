/* __isPlatformVersionAtLeast: what clang emits for Objective-C's @available() / C's __builtin_available(). Apple's
 * toolchain takes it from its compiler runtime (libclang_rt.osx.a), which upstream clang on Linux does not have; SDL2's
 * Cocoa code uses @available. Same approach as that runtime on macOS 10.15+: ask the system through
 * _availability_version_check (libSystem, via libxpc). Version encoding: major << 16 | minor << 8 | subminor. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
   uint32_t platform;
   uint32_t version;
} dyld_build_version_t;

extern bool _availability_version_check(uint32_t count, dyld_build_version_t versions[]);

int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major, uint32_t minor, uint32_t subminor);

int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major, uint32_t minor, uint32_t subminor)
{
   dyld_build_version_t v = {platform, ((major & 0xffff) << 16) | ((minor & 0xff) << 8) | (subminor & 0xff)};
   return _availability_version_check(1, &v);
}
