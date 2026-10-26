# StrixArc

**Metal on the AMD Radeon 880M / 890M (Strix Point, RDNA 3.5, GC 11.5.0) for macOS Sequoia 15.**
GPU bring-up, a display path WindowServer composites on, and Metal over Vulkan (Mesa RADV).

StrixArc is a port of [nullarc](https://github.com/luuumi-dev/nullarc) (Radeon 780M, Phoenix) to Strix Point. The
code keeps nullarc's component names (PhxBringup, PhxAccel, NVMTLDriver) and its licenses; see [LICENSE](LICENSE).
It is not affiliated with or endorsed by the nullarc project.

**Русский: [README_RU.md](README_RU.md)**

## Status

Measured on the reference machine (Lenovo laptop, Ryzen AI with Radeon 880M, 3072x1920 30-120 Hz eDP panel,
macOS 15.8.1) in October 2026:

| | |
|---|---|
| **GPU bring-up** | PSP (with a driver-owned TMR), SMU, RLC autoload, MES scheduler + KIQ, compute and gfx queues, at every boot (`phxboot` LaunchDaemon, with fail-safes). |
| **Metal** | Metal on Vulkan works for apps; WindowServer composites on the GPU. |
| **Display** | Native 3072x1920 at 60 Hz. Each composited frame is copied by **SDMA** into one of two buffers in the carve-out (0.84 ms for a 3K frame, 28 GB/s) and the display flips to it at vblank: 61.7 fps at 3K, no tearing, no CPU in the copy. The display engine (DCN 3.5) cannot read system RAM under macOS (its rIOMMU is never enabled). |
| **Not yet** | 120 Hz (needs a link retrain at HBR3 through DMCUB), video decode/encode, external displays, backlight control. |

## How the frame reaches the panel

```
WindowServer --Metal--> NVMTLDriver --Vulkan--> RADV --submit--> PhxBringup (gfx/compute queues)
     |
     '-- display pipe transaction --> PhxAccel (PhxAMDFB) --phxScanoutCopy--> PhxBringup:
             wait for the GPU work before the frame, SDMA COPY_LINEAR into the back buffer (VMID 7 page tables),
             check 32 pixels against the frame, flip DCN pipe 0 at vblank (DCSURF_PRIMARY_SURFACE_ADDRESS)
```

Any failure on that path puts the firmware's frame back on the panel and the CPU copy takes over until reboot.
The details, with every measurement, are in [docs/strix-point.md](docs/strix-point.md).

## Layout

| Path | What |
|---|---|
| `src/phxbringup` | PhxBringup.kext (in OpenCore `EFI/OC/Kexts`): PSP/SMU/MES bring-up, per-process GPUVM, the RADV submit ABI, SDMA scan-out copies |
| `src/phxaccel` | PhxAccel.kext (Auxiliary KC): IOAccelerator, PhxAMDFB framebuffer, display pipe |
| `src/nmplugin` | NVMTLDriver.bundle: the Metal plugin (Metal on Vulkan) |
| `src/amd/rdna3_chip.h` | per-chip profiles (Phoenix, Strix Point) |
| `tools/phxctl` | bring-up and test tools for the Mac: `mesboot`, `phxctl`, `phxboot.sh`, `fps_check.sh`, `go.sh` ... |
| `tools/cross/build-macos.sh` | builds the kexts and `mesboot` on Linux (upstream clang, cctools-port ld64, MacKernelSDK) |
| `tools/linux-trace` | Linux amdgpu register/DMUB traces of the 880M used as the reference |

## Building and testing

Build on Linux: `tools/cross/build-macos.sh <toolchain env.sh>`. A test round is a folder on the OpenCore USB stick
(`nullarc/` with `go.sh`, `TESTS`, `NOTE`); on the Mac `bash go.sh` installs what changed, asks for a reboot when one is
needed, otherwise runs the round's tests, and every script saves its logs on the stick (`nullarc_logs/`).

## License

Mixed, as in nullarc: MIT or GPL-2.0-only for the bring-up and tools, PolyForm Noncommercial 1.0.0 for the parts
derived from NullMoth's driver (noncommercial use only), MIT for the Mesa patches and the files taken from Linux. See
[LICENSE](LICENSE), [LICENSES/](LICENSES) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The nullarc name and
mark are not part of this repository.
