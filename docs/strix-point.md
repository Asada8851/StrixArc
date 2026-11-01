# Strix Point (Radeon 880M / 890M) — port status

Branch `strix-point-gfx1150`. **Nothing here has run on Strix Point hardware.** Everything was derived from the
Linux `amdgpu` sources (drm-next, October 2026), linux-firmware (October 2026) and Mesa at nullarc's base commit, and
checked only with unit tests and dry runs. The kext refuses to start on Strix Point unless the boot-args carry
`-phxexperimental`.

## What Strix Point has in common with Phoenix

Linux drives GC 11.5.0 through the same code as GC 11.0.1 almost everywhere (`gfx_v11_0.c`, `imu_v11_0.c`,
`mes_v11_0.c`, `sdma_v6_0.c`), so the bring-up order in `bringup5.sh` and `mesboot.c` carries over:

| | Phoenix (780M) | Strix Point (880M/890M) | Same handling? |
|---|---|---|---|
| PCI ID | 1002:1900 | 1002:150E | — |
| GC | 11.0.1 | 11.5.0 | yes, but registers moved (below) |
| PSP | 13.0.4 (`psp_v13_0_4.c`) | 14.0.0 (`psp_v13_0.c`) | ring protocol yes; driver-owned TMR on both, Strix's is 0x8900000 bytes |
| SMU | 13.0.4 | 14.0.0 | yes: same mailbox offsets, same message IDs for everything nullarc sends (0x01–0x03, 0x12, 0x16, 0x19, 0x1A) |
| SDMA | 6.0.1 | 6.1.0 | same header layout |
| MMHUB | 3.0.1 | 3.3.x (`mmhub_v3_3.c`) | the carve-out registers (`MMMC_VM_FB_*`) sit 0x100 dwords higher |
| DCN | 3.1.4 | 3.5.0 | OTG_CONTROL and the frame counter moved by one dword |
| NBIO | 7.7 | 7.11 | HDP flush remap register moved to another segment |
| Firmware | `gc_11_0_1_*`, `psp_13_0_4_toc` | `gc_11_5_0_*`, `psp_14_0_0_toc` | same header formats; different sizes and offsets |
| Mesa canned device | `phoenix` | `strix1` (already in Mesa) | `strix1` has 16 CUs (the 890M) |

## What changed in this branch

- **`src/amd/rdna3_chip.h`** — a per-chip profile (PCI ID, GC version, model name, firmware names, the GCVM/GCMC
  registers, the OTG registers, the HDP remap register). Plain C so the kext, `mesboot` and the tests share it.
  On GC 11.5 the whole GCVM/GCMC block sits 4 dwords higher than on 11.0; a few registers inside that range moved
  by other amounts (+1, +146), so translation is by table, never by offset arithmetic.
- **`tools/check_chip_regs.py <linux tree>`** — re-checks every table entry by name against `gc_11_0_0`,
  `gc_11_5_0`, `dcn_3_1_4`, `dcn_3_5_0`, `mmhub_3_0_1`, `mmhub_3_3_0`, and runs the Phoenix→Strix translation over all 263 GC 11.0 registers in
  the GCVM window: 207 land on the 11.5 register of the same name, 56 are refused, 0 are wrong.
- **PhxBringup** — picks the profile from the PCI device ID; the carve-out layout (MMHUB), per-VMID page tables and
  TLB flushes use the profile's registers; `model`, GC version and GB_ADDR_CONFIG come from it; publishes
  `phx-chip`; matches 1002:150E.
- **PhxAMDFB** — reads OTG0 through the profile (DCN 3.5 offsets on Strix). NMXAccel / PhxAMDFB match 1002:150E.
- **mesboot** — all register addresses stay written in Phoenix terms; `rd()`/`wr()` translate the GCVM ones and stop
  on any GCVM register the table does not know. On Strix the VRAM carve-out address comes from
  `GCMC_VM_FB_OFFSET` and the system aperture / default page are computed the way `gfxhub_v11_5_0` does, instead
  of the Firebat's Linux-trace constants.
- **bringup5.sh** — the Phoenix path is unchanged (checked: the same phxctl calls as before). On Strix it loads the
  plan `fwplan` reads from the firmware headers, after LOAD_TOC and **SETUP_TMR**. (An earlier version skipped
  SETUP_TMR on the claim that Linux keeps `boot_time_tmr` for PSP 14.0.0. It does not: `psp_early_init` clears it for
  MP0 13.0.1/3/4/5/8/11 and 14.0.0/1/4 in every kernel from v6.10 to master; only 14.0.2/14.0.3, the dGPUs of
  `psp_v14_0.c`, keep the boot-time TMR. Linux on the 880M logs `reserve 0x8900000 from 0x80e0000000 for PSP TMR`.
  Without the TMR the PSP refused IMU_I (0xFFFF000B) and the gfx PFP faulted on its first INDIRECT_BUFFER: client CPG,
  VMID 0, read, a different address every boot, CP_STAT PFP_BUSY|UTCL2IU_BUSY with the IB fetch never started.)
  The TMR goes where Linux puts it, 512 MiB below the top of VRAM, sized from the LOAD_TOC response's `tmr_size`.
- **`tools/fwplan`** — prints the LOAD_IP_FW plan (file, offset, size, PSP type) for one chip's firmware. On
  Phoenix's files it reproduces every line of `bringup5.sh`. The extractor learned MES (scheduler and KIQ pipes).
- **Mesa `0007`** — GC 11.5.0 in `acd_canned_gc_version`, and a build-time default canned device
  (`-Dc_args=-DACD_DEFAULT_CANNED='"strix1"'`). Checked to apply on top of 0001's post-image only.
- **Tests** — `test_rdna3_chip` (C and C++), MES extraction, Strix firmware fixtures (`make fixtures` fetches them).
- **`tools/check_strix_discovery.py`** — checks a Strix IP discovery dump against every base nullarc assumes.

## First steps on a Strix Point machine

1. **Dump the IP discovery table under Linux** (a live USB is enough):
   `sudo cat /sys/kernel/debug/dri/0/amdgpu_discovery > strix_ip_discovery.bin`, then
   `tools/check_strix_discovery.py strix_ip_discovery.bin`. Every `DIFF` is an address to fix before going near
   macOS. While there, a register trace of amdgpu's init (`docs/linux-trace-plan.md`) is the reference the Phoenix
   bring-up was built against, and worth the same for Strix.
2. `make fixtures test`, `python3 tools/check_chip_regs.py <linux>`.
3. Build `phxctl`, `mesboot` (`-I../../src/amd`) and `fwplan`, put them in `~/gpu/phxctl`, the `gc_11_5_0_*`,
   `sdma_6_1_0.bin` and `psp_14_0_0_toc.bin` files in `~/gpu/fw`.
4. Boot with `phxdebug=1 -phxexperimental`, **without** the phxboot LaunchDaemon and without arming PhxAMDFB:
   run `bringup5.sh` by hand over SSH (it arms a 3-minute reboot), then `full.sh` steps one at a time.

## Open problems (in the order they will bite)

1. **HDP flush** (worked around). NBIO 7.11 moved `BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL` to segment 5 (absolute
   0x4048e4d), beyond BAR5. PhxBringup flushes through `BIF_BX_PF1_HDP_MEM_COHERENCY_FLUSH_CNTL` (NBIF base 2 + 0xf7
   = 0xe17, `rdna3_chip.hdp_flush_direct`) instead and publishes `phx-hdp-flush-src` = `direct`. PSP, MES and the
   compute queue work with it on the 880M (try2..try7).
2. **IP base segments** are Phoenix's until step 1 confirms them (MP0/MP1 0x16000, MMHUB 0x1A000, SMUIO 0x16A00,
   DCN 0x34C0, NBIF 0xD20). Linux addresses Strix's MP0/MP1 through base index 0 where Phoenix uses 1, so the
   segment list is ordered differently there even if the absolute addresses stay the same.
3. **CU count.** Mesa's `strix1` is the 890M (16 CUs); the 880M has 12. RADV uses the count for limits, not for
   dispatch masks, but the kext should report the hardware's harvest (`CC_GC_SHADER_ARRAY_CONFIG`) instead.
4. **PCI identity in Mesa 0002** stays 1002:1900 for any canned device but gfx1201: cosmetic, but Metal tools show
   the wrong device.
5. **SETUP_TMR on Phoenix**: `bringup5.sh` sends TMR system address words `0x0C000000 0x6A`, while the kext's own
   `pspCtl` op 1 computes `fbPhys + tmrOff` (Linux `amdgpu_gmc_vram_mc2pa`). On Strix `bringup5.sh` sends the Linux
   value (`fbPhys + tmrOff`) through `phxctl raw`, since `pspCtl` op 1 caps the TMR at 16 MiB at a fixed 20 MiB offset.
6. **Display.** PhxAMDFB keeps scanning out the firmware (GOP) mode; only its refresh-rate read is chip-specific.
   External displays, backlight and DCN programming are out of scope here as they are on Phoenix.

## Side finding for Phoenix

`bringup5.sh` hardcodes the sizes of the gc_11_0_1 files the reference machine has. In current linux-firmware
`gc_11_0_1_mec.bin` (267904 vs 267008 bytes of ucode), `gc_11_0_1_mes_2.bin` and `gc_11_0_1_mes1.bin` changed size:
with updated firmware in `~/gpu/fw`, the script would load truncated payloads. `fwplan` gives the right numbers
for whatever files are installed.

## Strix Point on hardware (2026-10-25)

PSP, SMU, RLC autoload, MES (scheduler + KIQ), compute through MES (kexttest, vkprobe 256/256) and gfx ring packets
work. The first INDIRECT_BUFFER on the gfx queue stalled the PFP (fault 0xd32: CPG, VMID 0, read, a different address
every boot; CP_STAT PFP_BUSY|UTCL2IU_BUSY, the IB fetch never started) until the bring-up did SETUP_TMR (see above).
Related changes:

- **mesboot** sends MES the GC/MMHUB/OSSSYS bases from the chip profile (`rdna3_chip.mes`; Strix's base 3 differs
  from Phoenix's) and Linux's masks (VMIDs 0xff00, gfx HQD 0x2, no oversubscription timer). `gpustat` prints the
  queue/CP state plus the CP code fetch bases (inside the TMR once it is set up), `CP_GFX_ERROR` and IB1.
- **PhxBringup 33**: a wait completes when the submission's own queue has passed it (`gpuSeqDone`), so a hung gfx job
  no longer makes compute fences look unsignalled; `seq_retired` stays "everything up to here". `phxctl tmr <size>`
  places Strix's TMR where Linux does.

## Building on Linux

`tools/cross/build-macos.sh <env.sh>` builds PhxBringup.kext and mesboot with upstream clang/lld, cctools-port ld64
(kexts: lld cannot write MH_KEXT_BUNDLE), MacKernelSDK and a macOS SDK; `env.sh` sets the paths. Upstream clang
lowers the kext's global destructors to `__cxa_atexit`, which the kernel does not export: PhxBringup.cpp provides
its own and runs them from `__mod_term_func`, and the script refuses a kext that still imports it.

## Metal acceptance and WindowServer on the 880M (2026-10-25)

With the boot-time LaunchDaemon (`install_boot.sh`) the GPU is up before anyone logs in, so the step scripts that did
their own bring-up (try1..try14) must not run any more: a second `psp destroy/create` or `mesboot gart/mes/kiqsched`
under a running NMXAccel rewrites the page tables, rings and MES state its clients use, and the next Metal client
hangs in `waitUntilCompleted`. `mesboot` (every step but `gpustat`, `dump`, `claim`) and `bringup5.sh` now stop when
NMXAccel runs (`MESBOOT_FORCE=1` / `BRINGUP_FORCE=1` override). A log line `radv-darwin: native kext: ... build 32`
means the Mac booted an EFI with the old kext; everything below needs build 33.

- **`mtlprobe stress [s] [w h]`** — WindowServer's kind of work in one process, every result checked on the CPU:
  display-sized BGRA8 textures, buffer<->texture blits, source-over blending, compositing by `read()` and by a nearest
  `sample()`, a compute blur, IOSurfaces written by the CPU, drawn from and into by the GPU and read back by the CPU,
  3 queues x 8 command buffers in flight with completion handlers, and buffer/texture churn. Each step's name is
  printed before it runs, so a hang shows the step.
- **`try15.sh`** — on a phxboot boot, no bring-up: build >= 33, this boot's phxboot run reached `done`, the fault
  redirect is on (`GCVM_L2_PROTECTION_FAULT_CNTL` at 0x2828 = 0x3ffffffc), OTG0 on the DCN 3.5 offsets (0x4fea..0x500d,
  read only, checked against the 880M's IP discovery) runs and gives the refresh PhxAMDFB will measure; then compute,
  a draw, 120 s of stress, four stress processes at once, 20 draws in 20 new processes (new VMID contexts) and compute
  again. Passed: `/Users/Shared/gpu/ws-ready`.
- **`install_boot.sh --ws-trial`** — arms NVRAM `phxfb-boot` once and leaves `phxboot-m2` off, so phxboot does not
  re-arm: whatever the trial boot does (black panel, hang, panic), the boot after it is on the firmware framebuffer.
  Needs `ws-ready`, build 33 and automatic login. **`ws_check.sh`** then checks who has the panel (and PhxFB's reason
  if PhxAMDFB declined), phxboot's M2 run, frames presented through the GPU while it scrolls the Terminal, WindowServer
  reports since boot, and Metal beside WindowServer. Passed: `ws-trial-ok`, and `--windowserver` makes it every boot
  (`--no-windowserver` undoes it).
- Open: PhxAccel for macOS 15 is the non-`NM_TAHOE` build; M2 ran on the 780M under Tahoe 26.6 only. Under Linux the
  firmware puts BAR0 at 0x3000000000, under macOS PhxBringup saw it at 0x8b0000000: PhxAMDFB finds the GOP surface by
  content in whichever BAR holds it, and declines (firmware framebuffer) if none does.

## The 880M's panel and what a mode change takes (trace of Linux, 2026-10-26)

The panel (EDID, LEN 0x8ab1) is **3072x1920**, 30-120 Hz: 60 Hz is 387.8 MHz over 3232x2000 totals, 120 Hz the same
totals at 775.7 MHz (DisplayID). The firmware lights it at native 3072x1920@60 and scans a 1920x1200 GOP surface
scaled up (DSCL ratio 0x00a00000 = 1.6): that is what macOS gets today. `tools/linux-trace/trace_modeswitch.sh`
switches Linux (GNOME/Mutter, temporary configs) 120->60->120 Hz and native->1920x1200->native under ftrace;
`annotate_modeswitch.py` reads it. Every switch is a full DC commit (dc_commit_streams), the same sequence each time:

1. backlight off (DMUB VBIOS LVTMA_CONTROL 2 + DIG1_TRANSMITTER_CONTROL LCD_BLOFF), transmitter DISABLE;
2. eDP panel power off, ~0.5 s, on (LVTMA 13/12, TRANSMITTER POWER_OFF/POWER_ON);
3. SMU: DISPCLK 400 -> 800 MHz (msg 4), DPPCLK 390.6 -> 781.2 MHz (msg 6), DCFCLK deep sleep 27 -> 55 MHz (msg 8);
4. TRANSMITTER ENABLE at the link rate (symclk 432000 kHz = 4 lanes x 4.32 Gb/s at 60 Hz, 810000 kHz = HBR3 at
   120 Hz), link training: 16 x TRANSMITTER SETUP_VSEMPH with DPCD/AUX traffic;
5. backlight on.

PHY and panel power are DMCUB firmware work (VBIOS command tables run by DMUB); no REG_SEQ offload was seen, so
every other DC register write is in the trace. Registers that differ 60 -> 120 Hz, besides HUBP's DLG/TTU bandwidth
parameters: `DP_DTO0_PHASE` (exactly doubled), `OTG0_PIXEL_RATE_CNTL`, `DP0_DP_VID_M`, `DENTIST_DISPCLK_CNTL`,
`DP0_DP_SEC_CNTL1`; the OTG timing is untouched. Native vs 1920x1200 (both 60 Hz, same link): only the plane -
`HUBP0_DCSURF_PRI/SEC_VIEWPORT_DIMENSION` (0x07800c00 vs 0x04b00780), `HUBPREQ0_DCSURF_SURFACE_PITCH`, the surface
address and control, `DSCL0_SCL_MODE` (0 vs 0x101) with its ratios/taps, and the HUBP DLG/TTU set.

So for macOS: native 3072x1920@60 is a plane reprogramming (no clocks, no link); 120 Hz needs the SMU clocks, the
DTO, and a link retrain at HBR3 through DMCUB's transmitter commands plus AUX - i.e. a running DMCUB. Both need the
frame off the CPU first: a 3072x1920 frame through the BAR (~650 MB/s) is ~36 ms.

## Getting WindowServer's frame to the panel without the CPU copy (2026-10-26)

Today PhxAccel copies every composited frame (1920x1200, 9.2 MB) by CPU through the BAR into the firmware's
scan-out at FB offset 0 (~7.6 ms a frame). WindowServer's frames are IOSurface pages in system RAM
(`nvaccel_iop_src_surf` = every frame, `src_res` 0): the plugin renders there, PhxAccel's contiguous "VRAM" pool is
barely used. What `dcn_probe` / `mesboot dcnflip` found about the display's own reach:

- DCN translates addresses itself (`DCN_VM_FB_LOCATION`/`FB_OFFSET`, AGP, a VMID-0 page table); MMHUB is not in the
  path. A pipe needs Linux's per-pipe setting for anything outside the FB: system aperture over FB+AGP and
  `DCN_VM_MX_L1_TLB_CNTL` = 0x19 (the firmware leaves 0/0/0: an AGP address then goes out untranslated, white).
- After translation, a physical address inside `DCN_VM_LOCAL_HBM_ADDRESS_START..END` (16 MB units; the firmware: the
  carve-out 0x780..0x87f) goes straight to memory; any other goes through DCHVM to the SoC's rIOMMU. macOS never
  enables the AMD IOMMU, `dcn35_dchvm_init`'s HOSTVM_INIT_REQ never sees RIOMMU_ACTIVE, and such reads return all
  ones (white panel, three runs). The local range is not locked (`LOCK_CNTL` 0): widened over system RAM, DCN should
  read it like the carve-out, since on an APU it is the same DRAM (the next dcnflip run).

The ways to a zero-copy (or CPU-free) frame, in order of preference:

1. **Scan the IOSurface out of system RAM**: local range widened + a DCN VMID-0 page table (Linux's "sg display"
   layout: flat, depth 0) mapping each display surface's pages, flip = the pipe's surface address. No copy, no GPU
   work, any surface. Needs: the local-range trick to work, and PTEs for the 2-3 display surfaces WindowServer
   rotates (cached per surface).
2. **GPU copy into the carve-out scan-out**: a CP DMA_DATA (or SDMA) job from the surface's GPU address to FB
   offset 0, submitted by PhxBringup when PhxAccel presents: the display path stays as proven today, the CPU does
   nothing, ~1-3 ms of GPU time a frame. Needs a kernel-side submit on PhxBringup's ring and the surface's GPU VA.
3. **Render the display surface in the carve-out** (NVIDIA-style VRAM copy the plugin draws into, flipped there):
   DCN reads FB already, but the plugin would have to target VRAM for WindowServer's display surfaces.
4. **Turn the AMD IOMMU on in pass-through** so DCHVM/rIOMMU wake: system-wide, every device's DMA goes through it;
   last resort.
5. **Smaller CPU copies** (damage only): keeps the CPU in the loop; no help for 3K or 120 Hz.

### GPU copy into the scan-out (PhxBringup 34, 2026-10-26)

The widened `DCN_VM_LOCAL_HBM` range did not help either (white again): under macOS every DCN fetch outside the
carve-out ends at the rIOMMU. So the frame goes the other way round: the GPU, which reaches both system RAM and the
carve-out, copies it. PhxBringup's `phxScanoutCopy` (called by PhxAMDFB's `phxGpuCopy`, called by PhxAccel's display
pipe when `debug.nvaccel_iop_gpucopy` = 1) keeps a kernel GPUVM context of its own with the scan-out window
(the carve-out's physical pages, as system pages or - `debug.phxscanout_local` = 1 - as local memory) and the last
four frames' IOSurface pages mapped, writes one `DMA_DATA` (CP_SYNC, L2 source and destination, 9.2 MB) into a 4 KiB
IB and submits it on the compute queue; the fence `gpuSubmit` emits writes GL2 back, and the copy is waited for
(100 ms at most; a timeout turns GPU copies off until reboot). PhxAccel checks 64 pixels of every GPU-copied frame
on the panel (`nvaccel_iop_gpucopy_verify`); a mismatch has the CPU copy that frame (`nvaccel_iop_gpu_mismatch`).
`gpu_copy_test.sh` measures CPU vs GPU copy on the fpsanim window; `$G/phxboot-gpucopy` (and `-local`) turn it on at
boot.

Build 34 sent the whole frame as one DMA_DATA (9.2 MB) with CP_SYNC on the compute queue: GFX11 CP DMA takes at most
32767 bytes a packet (RADV `cp_dma_max_byte_count`; full size only behind the `radv_gfx11_full_size_cp_dma` drirc
option), and MEC needs no CP_SYNC (RADV "don't sync CP DMA on MEC"). The compute queue hung, WindowServer's compute
work with it, and phxboot took WindowServer off the GPU after three crashes. Build 35 copies in 32736-byte packets
(up to ~38 MB a frame in a 32 KiB IB) and has a self-test (`debug.phxscanout_selftest`: 128 KiB system to system,
checked by the CPU) that `gpu_copy_test.sh` runs before WindowServer's frames are touched - best on a boot where
WindowServer is on the firmware framebuffer, so that a hang costs only the test tools' Metal.

### SDMA copies and flips in the carve-out (mesboot sdmatest, then PhxBringup 36, 2026-10-26)

Build 35 never ran on the GPU: the boot after build 34's WindowServer trial hung ~80 s after phxboot armed the panel
(20:00; the GPU copy was not switched on), phxboot set its kill switch at the next boot, and every boot after that
skipped the bring-up ("kill switch present"). The unified log of that boot is what gpu_copy_test.sh collects next
(window 19:50-20:03).

`mesboot sdmatest` (SDMA0 queue 0 programmed by registers as `sdma_v6_0_gfx_resume_instance`, wptr by register, VMID 7
page tables in VRAM, nothing the running system uses touched) on the 880M, WindowServer on the firmware framebuffer:

| copy | 1920x1200 (9.2 MB) | 3072x1920 (23.6 MB) |
|---|---|---|
| system RAM -> carve-out, local PTEs | 0.32 ms (28.7 GB/s) | 0.84 ms (28.1 GB/s) |
| system RAM -> carve-out, snooped system PTEs | 0.33 ms | 0.84 ms |
| carve-out -> carve-out (VMID 0) | 0.33 ms | 0.85 ms |

Every frame checked. The global timestamp runs at ~100 MHz (82852 ticks for 0.84 ms). SDMA0's UCODE_VERSION reads 0
and F32 sits halted after the PSP loaded it; unhalting it is enough. Pipe 0 flipped to a second frame in the carve-out
(MC 0x80_0A80_0000) and back without an underflow; DCSURF_FLIP_CONTROL = 0x04100000, SURFACE_FLIP_TYPE 0: flips latch
at vblank (SURFACE_EARLIEST_INUSE showed the new address 0.5 ms after the write; FLIP_PENDING is the completion bit).
CONSTANT_FILL needs FILLSIZE 2 (bits 31:30) for a dword pattern; 0 is a byte fill.

PhxBringup 36 does the same from the display pipe: SDMA0 queue 0 at VRAM 0x6000000 (ring, write-back, IB), VMID 7 (not
one of the 8..15 lent to clients) over a page-table context in system memory, two scan-out buffers at 0x8000000 and
0xA000000 (32 MiB each), the frame's IOSurface pages mapped per slot. Per frame: wait for the last flip to land
(else the frame is dropped, kIOReturnBusy), one COPY_LINEAR into the back buffer, GCR + FENCE (50 ms at most, then the
engine is halted and soft-reset), 32 pixels compared with the source, flip. Any failure puts the firmware's frame back
first and switches the path off until reboot; PhxAccel then copies by CPU into the firmware's frame, as before. A
WindowServer hang can no longer come from this path: SDMA shares nothing with the gfx/compute queues. PhxBringup also
publishes TOM/TOM2/SYSCFG/MTRRdefType/PAT and the variable MTRRs (`phx-msr-*`): CPU writes into the carve-out run at
~55 MB/s under macOS, and Linux shows no variable MTRR above 4 GiB.

3K note: the PSP ring sits at VRAM 0x1100000, inside a 3072x1920 surface at offset 0 (22.5 MiB). With the frames in
the two scan-out buffers the firmware's surface only matters until the first flip, but the ring should move before a
3K GOP mode is used.

## 120 Hz under macOS: the link retrained at 8.1 Gb/s through the GOP's DMCUB (2026-10-27)

`mesboot hz 60|120 [again]` replays Linux DC's sequence above by registers and through DMCUB's INBOX1. The firmware
(the GOP) started DMCUB, so commands go to *its* DMCUB firmware, not Linux's: VBIOS LVTMA_CONTROL (type 128 sub 15)
and DIG1_TRANSMITTER_CONTROL (sub 1, `dig_transmitter_control_parameters_v1_7`, the bytes of the trace). The DLG/TTU
values come from the firmware's linear 3K@60 setup (Linux's are for its tiled surface): the refclk-per-pixel fields
halved for 120 Hz, `DP_DTO0_PHASE` doubled with the firmware's modulo, `DP0_DP_VID_M` scaled by pixel clock / link rate.
The firmware's 60 Hz values are saved once (`/Users/Shared/gpu/hz60.base`) and are what `hz 60` puts back.

What it took, round by round (hz_test.sh logs):

- **AUX**: each read of `AUX_SW_DATA` reports the index it came from (bits 20:16); the first read after the index write
  already comes from index 1. Taken in order, every DPCD read was one byte early.
- **DMCUB's trace buffer** (window 5: `dmub_debugfs_trace_header`, 16-byte entries code/tick/param0/param1; 0x21 starts
  a command with its VBIOS sub-type, 0x22 ends it) shows what the firmware did with each command. `mesboot` prints the
  new entries after every command.
- **Swing 3 stops the GOP's DMCUB for good** (laneset 07 at HBR3: INBOX1's read pointer never moved again, every later
  command - the way back included - queued up unseen). Lane settings are capped at swing 2 / pre-emphasis 2 now, with
  MAX_*_REACHED to the sink only; once a command is not taken, no more are sent.
- **The firmware's HPD handler.** TRANSMITTER ENABLE at 81000 (x10 kHz) did reprogram the PHY (trace: 0x00e8
  a8c0 -> 13c68, a rate-change sequence), but the GOP's DMCUB firmware answers every HPD event (trace 0x1a p0 9 /
  0x1b 0x10: after TRANSMITTER POWER_OFF, after POWER_ON, and once more in the middle of the rate change) by
  re-enabling the PHY with the link settings the GOP left it in region 4 (+0x5318 and +0x531c: symclk 43200; laneset 08
  at +0x5324; trace 0x3a = those words). So the PHY went back to 4.32 Gb/s by itself while the panel waited at 8.1:
  no lane locked. `hz` now moves those two words to the new symclk before the switch (and back with it); the handler's
  re-enable is then at the right rate and harmless.
- Linux also writes `BIOS_SCRATCH_6` (abs. 0x52: ACC_MODE 0x10, CRITICAL_STATE 0x100 during the modeset) and the PSP
  ring write pointer (`MPASP_SMN_C2PMSG_67`, abs. 0x16083: the DTM/HDCP TA on stream enable); neither was needed.

Result: clock recovery in one step, equalization in three (laneset 00 -> 08, the panel's own request), 1.5 s for the
whole switch, OTG at 119.9 Hz, no underflow, and the way back to 60 Hz just as clean.

At boot (`$G/phxboot-120`), phxboot runs `HZ_QUIET=1 mesboot hz 120` while PhxAMDFB still holds WindowServer back
(the busy hold), then `sysctl debug.phxfb_remeasure=1`: PhxAMDFB measures OTG0 again (30 frames, snapped to 120.000 Hz),
so the display mode, the VBL tick and the AGDC panel timing say 120 Hz before WindowServer opens the panel. macOS
then reports "3072 x 1920 @ 120.00Hz" and composites at 120 fps (fps_check: 122 frames/s presented, the test window
119.1-119.6 moves/s, 2.0-2.5 ms per frame in the display pipe; WindowServer 26-47 % of a core). A failed switch puts
60 Hz back and removes the flag; `$G/hz120.pending` stays until WindowServer's 90 s watch is over, so a hang or the
power button held on a dark panel turns 120 Hz off for the next boot.

Still slow: some heavy animations (Launchpad folders opening) drop to 25-60 fps (not measured per frame yet). A likely
part of it: the display transaction waits synchronously for the frame's GPU work, the previous flip and the copy
(2-2.5 ms of the 8.3 ms budget); the next step is to queue the copy behind a fence (SDMA polling the composite's
completion) instead of waiting in WindowServer's present.

## Video: playback, then H.264/HEVC decoded on VCN 4.0.5 (2026-10-27 .. 11-01)

**Playback (phase 1).** User videos stayed black in every player while Metal and YUV surfaces worked: AVPlayer waits on
the audio clock, and the audio device never started (AppleHDA had no codec layout; the USB interface failed its IO).
With a working audio kext every player plays; Safari 1080p drops 291 of 3000 frames on the CPU decoder. Found on the
way: the Metal plugin copied every plane of a multi-plane IOSurface (video frames) into a private image and back at
each frame, and WindowServer held them until GPU memory ran out; planes are now host imports like single-plane
surfaces (`NVMTL_NO_PLANE_HOST=1` restores the copies).

**VCN bring-up.** Strix Point has one VCN 4.0.5 (bases 0x7800, 0x7e00, 0x3c030000). It is brought up the way Linux
does in DPG mode, from the register trace of Linux on this machine (`tools/linux-trace/trace_vcn.sh`): MMHUB 3.3,
which macOS leaves off, set up like `mmhub_v3_3_gart_enable` with context 0 on the GC hub's GART table; the VCN
firmware through the PSP (GFX_FW_TYPE_VCN = 13), SMU PowerUpVcn0 (0x07), the DPG SRAM image through the PSP
(GFX_FW_TYPE_VCN0_RAM = 49, 27 register pairs), then the unified ring (`mesboot vcn up`, POWER_STATUS 1 -> 0x105).
Under DPG the VCN's own registers read 0xdeadbeef; only the always-on ones (ring pointers, POWER_STATUS) are readable,
and the write pointer register alone starts the ring. Reading an MMHUB invalidation engine's semaphore register takes
the semaphore: a register snapshot that read ENG0_SEM froze Linux's VCN ring once.

**Jobs.** `mesboot vcn adopt` hands the ring to PhxBringup (build 43): clients submit AMDGPU_HW_IP_VCN_ENC (7, the
unified queue; 4 is VCE) IBs, which run in the client's VMID through MMHUB (every bind and TLB flush is mirrored
there while VCN is adopted). Each job is IB packets, a fence write and a pad to 64 dwords. The kext never gets the
MMHUB semaphore (its read returns 0 where mesboot's returns 1, cause unknown): flushes go ahead after 20 us, always
acked (`debug.phxvcn_sem_miss` / `phxvcn_ack_miss`).

**RADV.** Mesa patch 0009 gives the Darwin backend the unified VCN queue (HW_IP_INFO, firmware version, the video
capability tables of VCN 4.0); built with `MESA_VIDEO_CODECS=h264dec,h265dec`. RADV then offers a decode queue with
H.264 up to 4096x4096 and HEVC up to 8192x4352, as Linux's RADV does on this 880M. Two backend bugs showed up on the
way: compute advertised four rings while the kext runs one, so FFmpeg's copies on queues 1-3 were rejected and the
device lost (0010: one GFX and one compute ring); and a freed BO left the backend's table only after the kext had
freed its handle, so another thread could get the handle back first and fail ("returned BO handle twice", 0011).

**FFmpeg and ffplay.** FFmpeg 8.1.2 cross-built with Vulkan (`tools/cross/build-ffmpeg.sh`); it dlopens the Vulkan
loader, and `tools/vkshim` stands in for one: a `libvulkan.dylib` that answers from RADV's ICD entry point. ffplay
(SDL2 2.32, static, `tools/cross/build-sdl2.sh`) decodes on VCN without its Vulkan renderer (RADV cannot present to a
Cocoa window): `ffmpeg-patches/0001` copies each frame to memory and puts NV12 straight into an SDL texture.
`sdl-patches/0001` uploads through reused buffers instead of a new staging texture per update, which in the Metal
plugin had meant GPU memory allocated, mapped and freed through the kext two or three times a frame.

Results (`tools/phxctl/vcn_decode.sh`, rounds 54-58): every frame decoded on VCN is bit-identical to FFmpeg's CPU
decoder and to Linux's (H.264 1080p/4K, HEVC 4K 8- and 10-bit, a user clip). The decode alone of 300 frames of 4K
takes 0.9-1.4 s of CPU on VCN against 11-17 s on the CPU decoder (153-235 fps). Playing a 10 s 4K HEVC clip in ffplay:

| | ffplay CPU | dropped frames |
|---|---|---|
| CPU decoder | 12.2 s | 0 |
| VCN, round 55 (staging texture per update, the BO race) | 7.8 s | 23 |
| **VCN, round 58** | **3.4 s** | **0** |

10-bit HEVC costs more (9-10 s): SDL2 has no P010 texture, so the frame is converted to 8 bits by swscale.
WindowServer composites the window for another 2-3.5 s of CPU per clip.

Limits: only programs on this FFmpeg use VCN (`ffmpeg`, `ffplay`, `vcn_decode.sh play <file>`); VideoToolbox, and so
Safari, QuickTime and AVFoundation players, still decode on the CPU. VCN is brought up by the test script, not at
boot, and the RADV with 0009-0011 runs from the round folder; the installed one (WindowServer's) still has the BO race.
