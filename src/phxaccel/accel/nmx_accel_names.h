// PhxAccel: the NullMoth accelerator built for the Radeon 780M (Phoenix) that PhxBringup drives.
// Every class gets an NMX name: NVMTLDriver picks AMD/RADV when the accelerator conforms to "NMXAccel",
// and treats any registered "NVAccel" as an NVIDIA card.
#pragma once
#define NVAccel            NMXAccel
#define NVEventMachine     NMXEventMachine
#define NVDisplayMachine   NMXDisplayMachine
#define NVTask             NMXTask
#define NVMemoryMap        NMXMemoryMap
#define IOAccelNVRMCarrier IOAccelNMXCarrier
#define NVResource         NMXResource
#define NVSysMemory        NMXSysMemory
#define NVVidMemory        NMXVidMemory
#define NVSurface          NMXSurface
#define NVDisplayPipe      NMXDisplayPipe
#define NVGLContext        NMXGLContext
#define NV2DContext        NMX2DContext
#define NVCLContext        NMXCLContext
#define NVVideoContext     NMXVideoContext
