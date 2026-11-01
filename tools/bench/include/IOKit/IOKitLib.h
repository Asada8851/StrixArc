/* Host test bench (Linux): the part of macOS's IOKitLib.h the PhxBringup clients use, served by tools/bench/phxmock.c,
 * a model of PhxBringup's user client. Only declarations; no Apple code. */
#ifndef BENCH_IOKITLIB_H
#define BENCH_IOKITLIB_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>

typedef int kern_return_t;
typedef uint32_t mach_port_t, io_object_t, io_service_t, io_connect_t, task_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef const void *CFDictionaryRef;
typedef int IOReturn;

#define kIOMainPortDefault 0u
#define kIOMapAnywhere 1u
#define kIOReturnSuccess 0
#define kIOReturnNoMemory ((int)0xe00002bd)
#define kIOReturnNoResources ((int)0xe00002be)
#define kIOReturnBadArgument ((int)0xe00002c2)
#define kIOReturnUnsupported ((int)0xe00002c7)
#define kIOReturnNotPrivileged ((int)0xe00002c1)
#define kIOReturnBusy ((int)0xe00002d5)
#define kIOReturnNotReady ((int)0xe00002d8)
#define kIOReturnNotFound ((int)0xe00002f0)

#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t c)
{ struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

mach_port_t mach_task_self(void);
CFDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t svc, task_t task, uint32_t type, io_connect_t *conn);
kern_return_t IOServiceClose(io_connect_t conn);
kern_return_t IOObjectRelease(io_object_t o);
kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, const void *inStruct,
                                  size_t inStructCnt, uint64_t *out, uint32_t *outCnt, void *outStruct, size_t *outStructCnt);
kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, uint64_t *out,
                                        uint32_t *outCnt);
kern_return_t IOConnectCallStructMethod(io_connect_t c, uint32_t sel, const void *in, size_t inCnt, void *out, size_t *outCnt);
kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_t task, mach_vm_address_t addr);
kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_t task, mach_vm_address_t *addr, mach_vm_size_t *size,
                                   uint32_t options);
#endif
