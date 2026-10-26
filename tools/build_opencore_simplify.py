#!/usr/bin/env python3
import sys
import os
import shutil
import importlib

sys.path.insert(0, "/tmp/OpCore-Simplify")

def run_build():
    from Scripts import utils
    # Monkey patch request_input at class level to run completely unattended
    def mock_input(self, prompt="", *args, **kwargs):
        p = str(prompt).lower()
        if "yes/no" in p or "y/n" in p:
            return "yes"
        return ""
    utils.Utils.request_input = mock_input

    mod = importlib.import_module("OpCore-Simplify")
    o = mod.OCPE()

    report_path = "./hardware/firebat_u6c_sysreport/Report.json"
    acpi_dir = "./hardware/firebat_u6c_sysreport/ACPI"

    print("Loading hardware report and ACPI...")
    hardware_report = o.u.read_file(report_path)
    o.ac.read_acpi_tables(acpi_dir)
    if not o.ac.ensure_dsdt():
        raise RuntimeError("Failed to load DSDT from ACPI directory!")

    print("Running Compatibility Checker...")
    hardware_report, native_v, ocl_v = o.c.check_compatibility(hardware_report)
    print(f"Native macOS range: {native_v}, OCLP range: {ocl_v}")

    # Target macOS Sonoma 14.x / Sequoia 15.x
    macos_version = "23.99.99"
    print(f"Targeting macOS Darwin version: {macos_version} (macOS Sonoma 14)")

    print("Customizing hardware devices...")
    customized_hardware, disabled_devices, needs_oclp = o.h.hardware_customization(hardware_report, macos_version)

    print("Selecting SMBIOS...")
    smbios_model = o.s.select_smbios_model(customized_hardware, macos_version)
    print(f"Selected SMBIOS model: {smbios_model}")

    print("Selecting ACPI patches...")
    o.ac.select_acpi_patches(customized_hardware, disabled_devices)

    print("Selecting kexts...")
    needs_oclp = o.k.select_required_kexts(customized_hardware, macos_version, needs_oclp, o.ac.patches)

    print("Configuring SMBIOS-specific options...")
    o.s.smbios_specific_options(customized_hardware, smbios_model, macos_version, o.ac.patches, o.k)

    print("Gathering OpenCorePkg and Kexts...")
    o.o.gather_bootloader_kexts(o.k.kexts, macos_version)

    print("Building OpenCore EFI...")
    o.build_opencore_efi(customized_hardware, disabled_devices, smbios_model, macos_version, needs_oclp)

    out_dir = "./hardware/efi_firebat_u6c_simplify"
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    shutil.copytree(o.result_dir, out_dir)
    print(f"Successfully generated OpCore Simplify EFI at: {out_dir}")

if __name__ == "__main__":
    run_build()
