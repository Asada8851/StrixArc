#!/usr/bin/env python3
"""
tests/test_rdna3_kext_bundle.py — Verification of RDNA3Driver.kext bundle, Mach-O binary, and OpenCore integration.
"""

import os
import sys
import struct
import plistlib
import subprocess

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEXT_DIR = os.path.join(PROJECT_ROOT, "hardware", "efi_firebat_u6c_simplify", "EFI", "OC", "Kexts", "RDNA3Driver.kext")
EXEC_PATH = os.path.join(KEXT_DIR, "Contents", "MacOS", "RDNA3Driver")
PLIST_PATH = os.path.join(KEXT_DIR, "Contents", "Info.plist")
CONFIG_PATH = os.path.join(PROJECT_ROOT, "hardware", "efi_firebat_u6c_simplify", "EFI", "OC", "config.plist")
OCVALIDATE = "/tmp/opencore_extracted/Utilities/ocvalidate/ocvalidate.linux"

def test_kext_structure():
    print("Testing Kext bundle directory structure...")
    assert os.path.isdir(KEXT_DIR), f"Kext dir does not exist: {KEXT_DIR}"
    assert os.path.isfile(EXEC_PATH), f"Executable missing: {EXEC_PATH}"
    assert os.path.isfile(PLIST_PATH), f"Info.plist missing: {PLIST_PATH}"
    print("[PASS] Kext directory structure valid")

def test_macho_header():
    print("Testing Mach-O 64-bit binary header...")
    with open(EXEC_PATH, "rb") as f:
        header = f.read(32)
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack("<IIIIIIII", header)

    assert magic == 0xfeedfacf, f"Expected MH_MAGIC_64 (0xfeedfacf), got {hex(magic)}"
    assert cputype == 0x01000007, f"Expected CPU_TYPE_X86_64 (0x1000007), got {hex(cputype)}"
    assert filetype == 0x0B, f"Expected MH_KEXT_BUNDLE (0x0b), got {hex(filetype)}"
    assert (flags & 0x4) != 0, "Expected DYLDLINK flag set"
    assert (flags & 0x80) != 0, "Expected TWOLEVEL flag set"
    print(f"[PASS] Mach-O 64-bit kext bundle verified (filetype=0x{filetype:x}, {ncmds} load commands, flags=0x{flags:x})")

def test_info_plist():
    print("Testing Info.plist dictionary...")
    with open(PLIST_PATH, "rb") as f:
        pl = plistlib.load(f)

    assert pl.get("CFBundleIdentifier") == "org.drip.driver.RDNA3Driver"
    assert pl.get("CFBundleExecutable") == "RDNA3Driver"
    assert pl.get("OSBundleRequired") in ("Root", "Local-Root")
    with open(EXEC_PATH, "rb") as f:
        assert b"kmod_info" in f.read(), "_kmod_info symbol missing in kext executable"

    personalities = pl.get("IOKitPersonalities", {})
    assert "RDNA3Driver" in personalities, "Missing RDNA3Driver personality"
    p = personalities["RDNA3Driver"]
    assert p.get("IOClass") == "RDNA3Driver"
    assert "0x19001002" in p.get("IOPCIPrimaryMatch", "")
    assert p.get("IOProviderClass") == "IOPCIDevice"
    assert p.get("IOUserClientClass") == "RDNA3UserClient"

    libs = pl.get("OSBundleLibraries", {})
    assert "com.apple.kpi.iokit" in libs
    assert "com.apple.iokit.IOPCIFamily" in libs
    print("[PASS] Info.plist personality and dependencies verified")

def test_opencore_config():
    print("Testing OpenCore config.plist registration...")
    with open(CONFIG_PATH, "rb") as f:
        pl = plistlib.load(f)

    kexts = pl.get("Kernel", {}).get("Add", [])
    entry = None
    entry_idx = -1
    for idx, k in enumerate(kexts):
        if k.get("BundlePath") == "RDNA3Driver.kext":
            entry = k
            entry_idx = idx
            break

    assert entry is not None, "RDNA3Driver.kext not found in Kernel -> Add"
    assert entry.get("Enabled") is True, "RDNA3Driver.kext is not enabled"
    assert isinstance(pl["Kernel"]["Quirks"]["PanicNoKextDump"], bool), "Expected PanicNoKextDump to be boolean"

    # Check that SSDT-PLUG-ALT is removed
    for a in pl.get("ACPI", {}).get("Add", []):
        assert "SSDT-PLUG-ALT" not in a.get("Path", ""), f"SSDT-PLUG-ALT should be removed, found: {a.get('Path')}"

    print("[PASS] OpenCore config.plist registration and quirks verified")

def test_ocvalidate():
    print("Running ocvalidate on OpenCore config...")
    if not os.path.exists(OCVALIDATE):
        print("[SKIP] ocvalidate not available at", OCVALIDATE)
        return
    res = subprocess.run([OCVALIDATE, CONFIG_PATH], capture_output=True, text=True)
    assert res.returncode == 0, f"ocvalidate failed with return code {res.returncode}:\n{res.stdout}\n{res.stderr}"
    assert "No issues found" in res.stdout, f"ocvalidate reported issues:\n{res.stdout}"
    print("[PASS] ocvalidate passed with 0 issues")

def test_opencore_vtable_relocations():
    print("Testing OpenCore KXLD Vtable relocation invariants...")
    with open(EXEC_PATH, "rb") as f:
        data = f.read()

    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from("<IIIIIIII", data, 0)
    off = 32
    sects = []
    symoff = nsyms = stroff = strsize = 0
    dysym = None
    for _ in range(ncmds):
        c, csize = struct.unpack_from("<II", data, off)
        assert c not in (0x80000034, 0x80000033), f"Forbidden modern dyld command {hex(c)} present in kext"
        if c == 0x19:
            segname = data[off + 8:off + 24].rstrip(b"\x00").decode()
            assert segname != "__DATA_CONST", "Forbidden __DATA_CONST segment in kext"
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = struct.unpack_from("<QQQQIIII", data, off + 24)
            for s in range(nsects):
                soff = off + 72 + s * 80
                sectname = data[soff:soff + 16].rstrip(b"\x00").decode()
                sseg = data[soff + 16:soff + 32].rstrip(b"\x00").decode()
                saddr, ssize, sfileoff, salign, sreloff, snreloc, sflags = struct.unpack_from("<QQIIIII", data, soff + 32)
                sects.append((sseg, sectname, saddr, ssize, sfileoff))
        elif c == 0x02:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
        elif c == 0x0B:
            dysym = struct.unpack_from("<18I", data, off + 8)
        off += csize

    assert dysym is not None, "Missing LC_DYSYMTAB"
    extreloff, nextrel, locreloff, nlocrel = dysym[14], dysym[15], dysym[16], dysym[17]
    assert nextrel > 500, f"Expected >500 external relocations, got {nextrel}"
    assert nlocrel > 20, f"Expected >20 local relocations, got {nlocrel}"

    def get_sym(idx):
        strx, stype, ssect, sdesc, sval = struct.unpack_from("<IBBHQ", data, symoff + idx * 16)
        end = data.find(b"\x00", stroff + strx)
        name = data[stroff + strx:end].decode()
        return (name, stype, ssect, sdesc, sval)

    syms = [get_sym(i) for i in range(nsyms)]
    sym_by_name = {s[0]: (i, s) for i, s in enumerate(syms) if (s[1] & 0x0E) != 0}
    assert "_kmod_info" in sym_by_name, "Missing defined _kmod_info symbol"

    ext_relocs = {}
    for i in range(nextrel):
        r_addr, r_info = struct.unpack_from("<iI", data, extreloff + i * 8)
        ext_relocs[r_addr] = (r_info & 0xFFFFFF, (r_info >> 28) & 0xF)

    loc_relocs = {}
    for i in range(nlocrel):
        r_addr, r_info = struct.unpack_from("<iI", data, locreloff + i * 8)
        loc_relocs[r_addr] = (r_info & 0xFFFFFF, (r_info >> 28) & 0xF)

    smcps = [s for s in syms if s[0].startswith("__ZN") and s[0].endswith("10superClassE")]
    assert len(smcps) == 2, f"Expected 2 SMCP symbols (RDNA3Driver, RDNA3UserClient), got {len(smcps)}"

    for smcp_name, stype, ssect, sdesc, sval in smcps:
        assert sval in ext_relocs, f"SMCP {smcp_name} at {hex(sval)} missing external relocation"
        super_meta_sym = syms[ext_relocs[sval][0]][0]
        assert super_meta_sym.endswith("10gMetaClassE"), f"Invalid super metaclass {super_meta_sym}"

    print(f"[PASS] OpenCore KXLD Vtable relocations verified (extrel={nextrel}, locrel={nlocrel})")


if __name__ == "__main__":
    print("=== Running RDNA3Driver Kext & EFI Verification Tests ===")
    test_kext_structure()
    test_macho_header()
    test_info_plist()
    test_opencore_vtable_relocations()
    test_opencore_config()
    test_ocvalidate()
    print("=== All Kext & EFI tests PASSED successfully ===")
