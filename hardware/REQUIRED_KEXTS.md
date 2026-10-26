# Required & Recommended Kexts for AMD Phoenix/Hawk Point (RDNA 3)

This document lists the required and recommended third-party OpenCore kernel extensions (kexts) to boot macOS Tahoe 26 (Darwin 25) on AMD Ryzen 7040/8040 (Phoenix, Hawk Point) laptops and mini PCs with the nullarc graphics driver. The reference machine is a Firebat U6C (Ryzen 7 H 255); other machines are untested.

> [!IMPORTANT]
> To keep this repository focused and lightweight, third-party precompiled binaries are not bundled. Download official release builds directly from the upstream repositories listed below.

---

## 1. Core System Kexts (Mandatory)

| Kext | Purpose | Official Release Repository |
| :--- | :--- | :--- |
| **Lilu.kext** | Essential kernel patching engine required by all other kexts. | [acidanthera/Lilu](https://github.com/acidanthera/Lilu/releases) |
| **VirtualSMC.kext** | Emulates Apple SMC chip. Its `SMCBatteryManager` plugin gives the battery readout on laptops. | [acidanthera/VirtualSMC](https://github.com/acidanthera/VirtualSMC/releases) |
| **RestrictEvents.kext** | Blocks unwanted system events, enables CPU name spoofing (`revpatch=cpuname,sbvmm`). | [acidanthera/RestrictEvents](https://github.com/acidanthera/RestrictEvents/releases) |
| **AMFIPass.kext** | Keeps apps that check AMFI working while the `amfi=0x80` boot-arg (needed to load nullarc into WindowServer) relaxes it; pair with `-amfipassbeta` on Tahoe. | [acidanthera/AMFIPass](https://github.com/acidanthera/AMFIPass/releases) |

---

## 2. Graphics Driver (This Project)

| Kext / Bundle | Purpose | Source / Download |
| :--- | :--- | :--- |
| **PhxBringup.kext** | OpenCore EFI kext (`EFI/OC/Kexts/`, needs `phxdebug=1`). Loads the GPU firmware through the PSP, powers up GFX, runs the MES scheduler and per-process GPUVM. | [Releases](https://github.com/luuumi-dev/nullarc/releases) or `src/phxbringup/build.sh` |
| **PhxAccel.kext** | `/Library/Extensions`, built into the Auxiliary kernel collection by the installer. The `NMXAccel` accelerator, the `PhxAMDFB` framebuffer and `PhxAGDC` display control. | [Releases](https://github.com/luuumi-dev/nullarc/releases) |
| **NVMTLDriver.bundle** + `nvmtl` | The Metal driver plugin and Mesa RADV, in `/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU` (linked from `/Library/GPUBundles`). | [Releases](https://github.com/luuumi-dev/nullarc/releases) |

---

## 3. Audio, Input & Peripherals

| Kext | Purpose | Official Release Repository |
| :--- | :--- | :--- |
| **AppleALC.kext** | Audio codec layouts (`layout-id` in `DeviceProperties`). macOS Tahoe no longer ships `AppleHDA.kext`, so analog audio also needs it restored (for example with OpenCore Legacy Patcher); nullarc does not ship it. | [acidanthera/AppleALC](https://github.com/acidanthera/AppleALC/releases) |
| **VoodooI2C.kext** | I2C bus driver for modern laptop trackpads and touchscreens. | [VoodooI2C/VoodooI2C](https://github.com/VoodooI2C/VoodooI2C/releases) |
| **VoodooI2CHID.kext** | Satellite driver for I2C HID precision trackpads (multitouch gestures). | [VoodooI2C/VoodooI2C](https://github.com/VoodooI2C/VoodooI2C/releases) |
| **BrightnessKeys.kext** | Automatic handling of ACPI brightness keys (F-keys). | [acidanthera/BrightnessKeys](https://github.com/acidanthera/BrightnessKeys/releases) |
| **USBPorts.kext** | Custom codeless USB port map tailored for your motherboard controller. | Map using [USBToolBox](https://github.com/USBToolBox/tool) or CorpNewt's USBMap |

---

## 4. Networking (Hardware-Dependent)

| Kext | Target Hardware | Official Release Repository |
| :--- | :--- | :--- |
| **AirportItlwm.kext** / **itlwm.kext** | Intel Wi-Fi cards (e.g. AX200 / AX210). | [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm/releases) |
| **IntelBluetoothFirmware.kext** + **IntelBTPatcher.kext** | Intel Bluetooth adapters. On Tahoe use a build with Tahoe support (2.5.1 or newer, e.g. [lshbluesky/IntelBluetoothFirmware](https://github.com/lshbluesky/IntelBluetoothFirmware/releases)) together with `BlueToolFixup.kext` from BrcmPatchRAM. | [OpenIntelWireless/IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware/releases) |
| **LucyRTL8125Ethernet.kext** | Realtek 2.5GbE Ethernet controllers. | [Mieze/LucyRTL8125Ethernet](https://github.com/Mieze/LucyRTL8125Ethernet/releases) |

---

## 5. Reference OpenCore Configuration

A clean reference configuration with dummy SMBIOS values is provided at:
👉 [`Sample.plist`](Sample.plist)

> [!WARNING]
> Before booting, generate your own unique SMBIOS identifiers (`SystemSerialNumber`, `MLB`, `SystemUUID`) using [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) with model `MacBookPro16,1`. Never use public or shared serial numbers.
