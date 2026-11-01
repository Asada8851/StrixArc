# StrixArc

**Metal on the AMD Radeon 880M / 890M (Strix Point, RDNA 3.5, GC 11.5.0) for macOS Sequoia 15.**
GPU bring-up, a display path WindowServer composites on, Metal over Vulkan (Mesa RADV), and H.264/HEVC decoded on
the GPU's video engine (VCN) in FFmpeg and its player.

StrixArc is a port of [nullarc](https://github.com/luuumi-dev/nullarc) (Radeon 780M, Phoenix) to Strix Point. The
code keeps nullarc's component names (PhxBringup, PhxAccel, NVMTLDriver) and its licenses; see [LICENSE](LICENSE).
It is not affiliated with or endorsed by the nullarc project.

**Русский: [README_RU.md](README_RU.md)**

> [!WARNING]
> **Tested on exactly one machine**: the author's Lenovo laptop with a Radeon 880M (Strix Point), macOS 15.8.1, booted
> through OpenCore from a USB stick. Nothing else has ever run this code. There is **no guarantee that it works on your
> machine, or at all**: it programs the GPU's firmware loader, memory controller and display engine directly, and a
> mistake can hang the Mac, panic it or leave the panel black until a reboot. Use it only if you can recover from that
> (boot from another EFI, `-phxbringupoff`, see [the way back](#switches-flags-and-the-way-back)). Provided as is, with
> no warranty of any kind.


## Status

Measured on the reference machine (Lenovo laptop, Ryzen AI with Radeon 880M, 3072x1920 30-120 Hz eDP panel,
macOS 15.8.1) in October 2026:

| | |
|---|---|
| **GPU bring-up** | PSP (with a driver-owned TMR), SMU, RLC autoload, MES scheduler + KIQ, compute and gfx queues, at every boot (`phxboot` LaunchDaemon, with fail-safes). |
| **Metal** | Metal on Vulkan works for apps; WindowServer composites on the GPU. |
| **Display** | Native 3072x1920 at **120 Hz** (v0.2.0): at boot the eDP link is retrained from the firmware's 4.32 Gb/s to 8.1 Gb/s through DMCUB, before WindowServer opens the panel. Each composited frame is copied by **SDMA** into one of two buffers in the carve-out (0.84 ms for a 3K frame, 28 GB/s) and the display flips to it at vblank: ~120 fps at 3K, no tearing, no CPU in the copy. The display engine (DCN 3.5) cannot read system RAM under macOS (its rIOMMU is never enabled). |
| **Video** (v0.2.5) | H.264 and HEVC (8/10-bit, up to 4K tested) decoded on VCN 4.0.5 through RADV's Vulkan Video, bit-identical to the CPU decoder, in FFmpeg 8.1 and its player: `vplay <file>` or **VCN Player.app** from Finder (4K playback: ~3.5 s of CPU per 10 s clip instead of 12.2 s, no dropped frames). VCN comes up at boot; `sysctl debug.phxvcn_jobs` and `~/Library/Logs/vplay.log` show it decoding. Safari, QuickTime and other VideoToolbox players still decode on the CPU. See [docs/strix-point.md](docs/strix-point.md). |
| **Not yet** | VCN decode for VideoToolbox (Safari, QuickTime), video encode, external displays, backlight control, variable refresh. Some heavy animations (Launchpad folders opening) still drop well below 120 fps. |

## Performance

`fps_check.sh` on the reference machine, 3072x1920: a test window (`fpsanim`) moves its square every 1/120 s for 6 s,
hands off; frames presented and the time each takes in the display pipe are PhxAccel's counters, WindowServer's CPU is
`top` over the run (percent of one core).

| Version / path | Refresh | Frames presented | Per frame in the display pipe | WindowServer CPU |
|---|---|---|---|---|
| before SDMA: the CPU copies through the BAR | 60 Hz | 30.8 fps | 19.2 ms (the copy) | 8.5-9.9 % |
| v0.1.0: SDMA copy + vblank flip | 60 Hz | 61.8 fps | 7.1 ms (mostly waiting for the previous flip) | 13.6 % |
| **v0.2.0: SDMA copy + vblank flip** | **120 Hz** | **121.8-122.3 fps** (the window: 119.1-119.6 moves/s) | **2.0-2.5 ms** | **26-47 %** |

The raw SDMA copy of a 3K frame is 0.84 ms (28 GB/s); the rest of the time per frame is waiting for the GPU work
before the frame, for the previous flip and the 32-pixel check. The presented count reads slightly above the refresh
because the counters are sampled over 6 s plus the time the sampling takes; the window's own count is exact.
macOS reports the panel as "3072 x 1920 @ 120.00Hz" and AppKit sees 120-121 fps.

## Differences from nullarc

nullarc targets the Radeon 780M (Phoenix, GC 11.0.1) under macOS Tahoe 26. StrixArc keeps its architecture and changes:

| | nullarc (780M) | StrixArc (880M) |
|---|---|---|
| GPU | GC 11.0.1, PCI 1002:1900 | GC 11.5.0, PCI 1002:150E; GCVM registers translated by table (`src/amd/rdna3_chip.h`), MES gets Strix's IP bases |
| PSP | 13.0.4, firmware-owned TMR | 14.0.0: the driver owns the TMR (`SETUP_TMR`, 0x8900000 bytes at 512 MiB below the top of VRAM), firmware plan read from the headers (`fwplan`), PSP ring at 48 MiB (out of a 3K frame) |
| HDP flush | NBIO remap register | NBIO 7.11 moved it past BAR5: flushed through `BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL` directly |
| VM faults | stall | redirected to a dummy page by default (the gfx CP reads one unmapped address at the first IB of every new VMID context) |
| Refresh | the 780M's firmware lights its panel at the native 120 Hz | the GOP lights the 880M's panel at 60 Hz (4.32 Gb/s); at boot `mesboot hz 120` retrains the link at 8.1 Gb/s through the GOP's DMCUB (VBIOS transmitter commands, SMU display clocks, DTO and DLG for 120 Hz) and PhxAMDFB measures the panel again ([below](#120-hz-how-the-switch-works)) |
| Display | the CPU copies each frame into the carve-out (fast on the 780M) at 120 Hz | CPU writes into the carve-out run at ~55 MB/s and through the BAR at ~600 MB/s (a 3K frame: 20-37 ms), and DCN 3.5 cannot scan system RAM (no rIOMMU under macOS): **SDMA copies the frame into a second buffer and the display flips to it** (below) |
| Resolution | the firmware's | native 3072x1920 through OpenCore `UEFI > Output > Resolution = Max` (the firmware used to scale 1920x1200) |
| macOS | Tahoe 26 | Sequoia 15 (PhxAccel's non-Tahoe build) |
| Build | Xcode on a Mac | also on Linux: upstream clang + cctools-port ld64 (`tools/cross/build-macos.sh`; a `__cxa_atexit` shim, no fortified libc calls) |
| Mesa | canned `phoenix` | patch 0007: GC 11.5.0 and the canned `strix1` device |
| Boot daemon | phxboot | plus: the 120 Hz switch before WindowServer opens the panel, SDMA copies from WindowServer's first frame, a hang inside the first minute of either switches only that off, an orderly reboot is not taken for a hang, logs of every boot are kept |

## SDMA scan-out: how it works and when it falls back

PhxBringup runs SDMA0 queue 0 itself (ring at VRAM 0x6000000, VMID 7 with its own page tables, two 32 MiB scan-out
buffers at VRAM 0x8000000 and 0xA000000). For every frame WindowServer presents, with `debug.nvaccel_iop_gpucopy=1`:

1. wait until the GPU has finished the work submitted before the frame (50 ms at most; past that the frame is copied
   anyway and `phxscanout_gpulate` counts it) - WindowServer hands frames over before their composite is done;
2. wait until the previous flip has landed (25 ms at most; else the frame is dropped, `phxscanout_busy`);
3. one `COPY_LINEAR` of the whole frame into the buffer that is not on the panel, a GL2 write-back and a fence;
4. 32 pixels of the copy compared with the frame (read through PhxAccel's mapping, cache lines flushed);
5. flip DCN pipe 0 to that buffer; it latches at the next vblank.

| What happens | Result |
|---|---|
| the source pixel still changes (the GPU is still drawing the frame) | frame dropped, the panel keeps the last one (`phxscanout_moving`) |
| a copied pixel is wrong | frame dropped (`phxscanout_mismatch`, `phxscanout_lastbad`); **8 in a row: off until reboot** |
| the copy does not finish in 50 ms | SDMA halted and soft-reset, **off until reboot** |
| pipe 0 underflows, or the frame size / the firmware frame changes | **off until reboot** |
| SDMA0 does not start, the buffers cannot be mapped | **off until reboot** |

"Off" always puts the firmware's frame back on the panel first; PhxAccel then copies by CPU (about 30 fps at 3K) until
the next boot. `sysctl debug.phxscanout_state` says `on` or why it is off; `debug.phxscanout_copies`, `_us_sum`,
`_gpuwait_us`, `_busy`, `_moving`, `_mismatch`, `_lastbad` count the rest. `sudo sysctl -w debug.nvaccel_iop_gpucopy=0`
switches back to the CPU copy at any time. `bash tools/phxctl/fps_check.sh` (in the test round: `go.sh`) measures both.

## 120 Hz: how the switch works

The firmware (GOP) trains the panel's link at 4.32 Gb/s (eDP 1.4 rate table index 5) - enough for 60 Hz only.
`mesboot hz 120` does what Linux DC does on this laptop (traced under Linux, `tools/linux-trace`), through the DMCUB the
GOP left running:

1. SMU display clocks: DISPCLK 800 MHz, DPPCLK 782 MHz, deep-sleep DCFCLK 55 MHz;
2. blank: DPG black, backlight off, the stream, OTG and DIG off, the sink to D3, transmitter off, panel power off;
3. timing: `DP_DTO0_PHASE` doubled, HUBP DLG/TTU from the firmware's 60 Hz values with the time fields halved;
4. panel power on, TRANSMITTER ENABLE at symclk 810 MHz, link training at rate index 7 (8.1 Gb/s): TPS1, TPS4, the lane
   setting the panel asks for (swing capped at 2);
5. stream on with `DP_VID_M` for the new rate, backlight on, picture.

The GOP's DMCUB firmware re-enables the PHY with the link settings the GOP left it on every HPD event, one of which
comes in the middle of the rate change: `mesboot` moves those settings (DMCUB region 4, +0x5318) to the new rate
first, and back with `hz 60`. The details are in [docs/strix-point.md](docs/strix-point.md).

At boot, with `$G/phxboot-120`, phxboot does this while PhxAMDFB still holds WindowServer back, then
`sysctl debug.phxfb_remeasure=1` has PhxAMDFB measure the panel again, so the mode and the VBL tick say 120 Hz
when WindowServer opens it.

| What happens | Result |
|---|---|
| the switch fails (no lock at 8.1 Gb/s, DMCUB stops taking commands) | back to 60 Hz the same way, `phxboot-120` removed (60 Hz from then on) |
| the way back fails too (the panel stays dark) | phxboot reboots; the GOP lights the panel at 60 Hz again |
| a hang, or the power button held on a dark panel, within WindowServer's first 90 s | `hz120.pending` is found at the next boot: `phxboot-120` removed |

`bash gpu/hz_test.sh` switches 60 -> 120 -> 60 Hz by hand (logs every DMCUB command and its trace); `bash gpu/hz_ws_check.sh`
compares the link, PhxAMDFB, macOS's display settings and the frame counters at 120 Hz.

## Switches, flags and the way back

Flags are files in `/Users/Shared/gpu` (`$G`), read by the boot daemon (`/var/log/phxboot.log` says what it did). Script
names are those of a test round folder; in the repository the Strix versions are `tools/phxctl/*_strix.sh`.

| Flag | Meaning | Set by / undo |
|---|---|---|
| `phxboot-off` | kill switch: phxboot does nothing, the GPU stays off | set automatically when a boot did not finish its bring-up or WindowServer watch; `sudo bash install_boot.sh` (or `go.sh`) clears it |
| `phxboot-m2` | WindowServer on the GPU (PhxAMDFB takes the panel) | `install_boot.sh --windowserver [--force]` / `--no-windowserver`; removed after 3 WindowServer crashes in 90 s or 3 GPU-hang reboots within an hour |
| NVRAM `phxfb-boot` | arms PhxAMDFB for the next boot | re-armed by phxboot after every good M2 boot; `sudo nvram -d phxfb-boot` |
| `phxboot-gpucopy` | SDMA copies at boot | created when `fps_check.sh` sees SDMA pass; `sudo rm` it to go back to the CPU copy |
| `gpucopy.pending` | the SDMA minute of this boot is running | automatic; found at the next boot it means a hang: `phxboot-gpucopy` is removed |
| `phxboot-120` | 120 Hz at boot | `go.sh` creates it once for a release or round folder that has the file `HZ120`; phxboot removes it after a failed switch; `sudo rm` it for 60 Hz |
| `hz120.pending` | the 120 Hz switch of this boot is not through yet (until WindowServer's 90 s watch) | automatic; found at the next boot it means a hang: `phxboot-120` is removed |
| `hz60.base` | the firmware's 60 Hz display values, saved by the first `mesboot hz` | automatic; what `hz 60` puts back |
| `phxboot-apps` | Metal for every app when WindowServer is not on the GPU | `sudo touch` / `sudo rm` |
| `phxboot-vcn` | VCN at boot: up and handed to PhxBringup after WindowServer's watch, so players decode on it | `go.sh` creates it once for a release or round folder that has the file `VCNBOOT`; phxboot removes it after VCN failed to come up; `sudo rm` it |
| `vcn.pending` | VCN is coming up this boot (20 s) | automatic; found at the next boot it means a hang: `phxboot-vcn` is removed |
| `radv.pending` | a new RADV on trial (`update_radv.sh`): `new`, then `tried` once a boot runs WindowServer on it | phxboot removes it once WindowServer got through its 90 s watch; a boot on it that did not puts the previous RADV back |
| `logs/*-atboot` | each boot's logs, ~100 s after boot (the last 6) | automatic; `nullarc_logs.sh` copies them to the USB stick |

Backups and how to undo each change:

| Change | Undo |
|---|---|
| PhxBringup.kext in `EFI/OC/Kexts` on the stick | each update leaves `EFI-backup-<date>-build<N>/PhxBringup.kext` at the stick's root: copy it back; boot-arg `-phxbringupoff` disables PhxBringup entirely |
| PhxAccel.kext in the Auxiliary KC | `sudo bash update_accel.sh --undo` (backups in `/Users/Shared/nullarc-backup/accel-update.*`) |
| native 3K (OpenCore `Resolution=Max`) | `EFI/OC/config-1920x1200.plist` is the config from before: copy it over `config.plist` |
| the boot daemon | `sudo bash install_boot.sh --undo` (the GPU is no longer brought up at boot) |
| HiDPI modes | `sudo bash gpu/hidpi.sh --undo` |
| the RADV (`/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib`) | automatic if WindowServer fails on a new one; else `sudo bash ~/gpu/update_radv.sh --undo` and reboot (backups in `/Users/Shared/nullarc-backup/radv-update.*`) |
| VCN at boot | `sudo rm /Users/Shared/gpu/phxboot-vcn` and reboot |
| the player (`/Users/Shared/gpu/ffmpeg`, `/usr/local/bin/vplay`, `/Applications/VCN Player.app`) | `sudo bash ~/gpu/install_player.sh --undo` |
| 120 Hz | `sudo rm /Users/Shared/gpu/phxboot-120` and reboot (or, this boot only: `sudo ~/gpu/phxctl/mesboot hz 60`; WindowServer keeps pacing at 120 until the reboot) |

## How the frame reaches the panel

```
boot:  phxboot --mesboot hz 120--> PhxBringup (DMCUB, AUX, SMU, DCN registers) --> link at 8.1 Gb/s, OTG at 120 Hz;
       sysctl debug.phxfb_remeasure=1 --> PhxAMDFB reports 120 Hz; then WindowServer opens the panel

WindowServer --Metal--> NVMTLDriver --Vulkan--> RADV --submit--> PhxBringup (gfx/compute queues)
     |
     '-- display pipe transaction --> PhxAccel (PhxAMDFB) --phxScanoutCopy--> PhxBringup:
             wait for the GPU work before the frame, SDMA COPY_LINEAR into the back buffer (VMID 7 page tables),
             check 32 pixels against the frame, flip DCN pipe 0 at vblank (DCSURF_PRIMARY_SURFACE_ADDRESS)
```

The details, with every measurement, are in [docs/strix-point.md](docs/strix-point.md).

## Layout

| Path | What |
|---|---|
| `src/phxbringup` | PhxBringup.kext (in OpenCore `EFI/OC/Kexts`): PSP/SMU/MES bring-up, per-process GPUVM, the RADV submit ABI, SDMA scan-out copies |
| `src/phxaccel` | PhxAccel.kext (Auxiliary KC): IOAccelerator, PhxAMDFB framebuffer, display pipe |
| `src/nmplugin` | NVMTLDriver.bundle: the Metal plugin (Metal on Vulkan) |
| `src/amd/rdna3_chip.h` | per-chip profiles (Phoenix, Strix Point) |
| `tools/phxctl` | bring-up and test tools for the Mac: `mesboot` (also `hz 60\|120`, `dpcd`, `dmub`, `dmubtrace`), `phxctl`, `phxboot.sh`, `fps_check.sh`, `hz_test.sh`, `hz_ws_check.sh`, `go.sh` ... |
| `tools/release/build_release_strix.sh` | the release package, from a test-round folder the reference machine ran |
| `tools/cross/build-macos.sh` | builds the kexts and `mesboot` on Linux (upstream clang, cctools-port ld64, MacKernelSDK) |
| `tools/linux-trace` | Linux amdgpu register/DMUB traces of the 880M used as the reference |

## Release package

`StrixArc-<version>.zip` (GitHub releases) is the test-round folder the reference machine ran - the binaries exactly as
tested (PhxBringup, PhxAccel, the NVMTLDriver binary, RADV, `mesboot`/`phxctl`/`fwplan`, FFmpeg/ffplay) - plus the 10
firmware files from linux-firmware (unmodified) and the licenses. NullMoth's bundle and its AIR translator are not in it:
`setup_metal.sh` takes them from NullMoth 2.4.1's package. Its `README.txt` gives the order: PhxBringup into OpenCore,
`bash install.sh`, `sudo bash setup_metal.sh <NullMoth pkgroot> .`, `bash go.sh`, reboot, `bash go.sh` again.

## Building and testing

Build on Linux: `tools/cross/build-macos.sh <toolchain env.sh>`. A test round is a folder on the OpenCore USB stick
(`nullarc/` with `go.sh`, `TESTS`, `NOTE`); on the Mac `bash go.sh` installs what changed, asks for a reboot when one is
needed, otherwise runs the round's tests, and every script saves its logs on the stick (`nullarc_logs/`).

## License

Mixed, as in nullarc: MIT or GPL-2.0-only for the bring-up and tools, PolyForm Noncommercial 1.0.0 for the parts
derived from NullMoth's driver (noncommercial use only), MIT for the Mesa patches and the files taken from Linux. See
[LICENSE](LICENSE), [LICENSES/](LICENSES) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The nullarc name and
mark are not part of this repository.
