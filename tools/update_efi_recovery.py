#!/usr/bin/env python3
import plistlib
import sys

def patch_config(path):
    print(f"[*] Patching {path}...")
    with open(path, "rb") as f:
        data = plistlib.load(f)

    # 1. Enable SSDT-RMNE.aml
    for acpi in data["ACPI"]["Add"]:
        if "SSDT-RMNE.aml" in acpi["Path"]:
            acpi["Enabled"] = True
            print("  [+] Enabled SSDT-RMNE.aml")

    # 2. Enable NullEthernet.kext, Disable AirportItlwm.kext
    for kext in data["Kernel"]["Add"]:
        if "NullEthernet.kext" in kext["BundlePath"]:
            kext["Enabled"] = True
            print("  [+] Enabled NullEthernet.kext")
        elif "AirportItlwm.kext" in kext["BundlePath"]:
            kext["Enabled"] = False
            print("  [-] Disabled AirportItlwm.kext")

    # 3. Add ipc_control_port_options=0 amfi=0x80 to boot-args
    nvram_guid = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
    boot_args = data["NVRAM"]["Add"][nvram_guid]["boot-args"]
    for arg in ["ipc_control_port_options=0", "amfi=0x80"]:
        if arg not in boot_args:
            boot_args += f" {arg}"
    data["NVRAM"]["Add"][nvram_guid]["boot-args"] = boot_args
    print(f"  [+] Updated boot-args: {boot_args}")

    with open(path, "wb") as f:
        plistlib.dump(data, f)
    print(f"[+] Successfully saved {path}")

if __name__ == "__main__":
    patch_config("hardware/efi_firebat_u6c_simplify/EFI/OC/config.plist")
    patch_config("/Volumes/OPENCORE/EFI/OC/config.plist")
