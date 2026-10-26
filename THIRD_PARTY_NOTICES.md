# Third-party notices

Components that nullarc's source tree or release packages carry, the license each comes under, and where its source
is. Full license texts are in [`LICENSES/`](LICENSES) unless noted.

## NullMoth macOS driver — `src/phxaccel`, `src/nmplugin`, `tools/nvmtl-patches`, `NVMTLDriver.bundle`

PolyForm Noncommercial License 1.0.0 — [`LICENSES/PolyForm-Noncommercial-1.0.0.txt`](LICENSES/PolyForm-Noncommercial-1.0.0.txt)

```
Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
```

nullarc's plugin and accelerator are NullMoth's code with changes for AMD hardware. Noncommercial use only.

## Shader translator — `NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib`

GNU Lesser General Public License v3.0 or later — [`LICENSES/LGPL-3.0-or-later.txt`](LICENSES/LGPL-3.0-or-later.txt),
which incorporates [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt).

Based on metal2vulkan, Copyright (C) Anees Iqbal (https://github.com/steelbrain/metal2vulkan), with modifications by
NullMoth Systems under the same license. The library is shipped unmodified, as built by NullMoth 2.4.1. Its
corresponding source (the `translator/` directory of NullMoth's driver, https://github.com/nullmoth/nvidia-macos-driver)
is attached to every nullarc release as `nvmtl-translator-source.tar.gz`. You may replace the library with your own
build: the plugin loads it with `dlopen` and calls its exported C functions only.

## LLVM and zstd — `NVMTLDriver.bundle/Contents/MacOS/air-runtime/`

`libLLVM.dylib`: Apache License 2.0 with LLVM Exceptions; `libzstd.1.dylib`: BSD-3-Clause (or GPL-2.0). Their license
files ship next to them in `NVMTLDriver.bundle/Contents/Resources/air-runtime/`. Source: https://github.com/llvm/llvm-project,
https://github.com/facebook/zstd.

## Vulkan loader — `nvmtl/libvulkan.1.dylib`

Apache License 2.0 — [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt). Copyright The Khronos Group Inc., Valve
Corporation, LunarG, Inc. Source: https://github.com/KhronosGroup/Vulkan-Loader. Shipped unmodified.

## Mesa (RADV) — `nvmtl/libvulkan_radeon.dylib`, `mesa-patches-phoenix`

MIT, Copyright The Mesa 3D Graphics Library contributors (https://docs.mesa3d.org/license.html). Built from Mesa
`f5cb8ee0` with Cobalt64's Darwin patches and [`mesa-patches-phoenix`](mesa-patches-phoenix) (source:
https://gitlab.freedesktop.org/mesa/mesa). A few Mesa source files carry other permissive licenses, as listed on
that page.

## Cobalt64 (Navi48-MacOS) — RADV Darwin backend, `src/phxbringup/PhxNativeABI.h` layouts

MIT License, Copyright (c) 2026 Navi48-MacOS contributors. The user-client ABI layouts and the RADV Darwin winsys
follow Cobalt64.

## Linux amdgpu — `tools/phxctl/mes_v11_api_def.h`, `tools/phxctl/csb_gfx11.h`

MIT-style license, Copyright 2021–2022 Advanced Micro Devices, Inc.; the notice is at the top of each file.
PhxBringup's bring-up sequences were written after the Linux amdgpu driver (MIT parts) and a register trace of it.

## AMD GPU microcode — `fw/*.bin` in the release package

From linux-firmware (https://gitlab.com/kernel-firmware/linux-firmware, `amdgpu/`), unmodified.
[`LICENSES/LICENSE.amdgpu.txt`](LICENSES/LICENSE.amdgpu.txt): Copyright (C) Advanced Micro Devices, Inc.;
redistribution in binary form only; no reverse engineering, decompilation or disassembly.

## Not included

- **Apple software.** Nothing from macOS or Xcode is redistributed. In particular NullMoth's `air-opt` (an Apple
  binary) and Apple's `AppleHDA.kext` are left out.
- **OpenCore and its kexts** (Lilu, VirtualSMC, …): get them from their projects; see
  [`hardware/REQUIRED_KEXTS.md`](hardware/REQUIRED_KEXTS.md).
- **Firmware images of the reference laptop.** The VBIOS and the ACPI VFCT table that holds it were removed from
  the tree: they are AMD/OEM copyright with no redistribution grant.
