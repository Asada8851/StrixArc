#!/usr/bin/env python3
"""
tools/build_kext.py — Build script for native RDNA3Driver.kext targeting macOS Sonoma/Tahoe.
Compiles freestanding C++17 modules and kmod_info glue into a unified Mach-O object using Clang,
and packs it into a classic MH_KEXT_BUNDLE with LC_DYSYMTAB extrel/locrel tables for OpenCore KXLD.
"""

import os
import sys
import subprocess
import struct
import shutil
import plistlib
import uuid

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK_HEADERS = os.path.join(PROJECT_ROOT, "reference", "MacKernelSDK", "Headers")
BUILD_DIR = os.path.join(PROJECT_ROOT, "build", "kext_obj")
KEXT_BUNDLE_DIR = os.path.join(PROJECT_ROOT, "build", "RDNA3Driver.kext")
DEST_KEXT_DIR = os.path.join(PROJECT_ROOT, "hardware", "efi_firebat_u6c_simplify", "EFI", "OC", "Kexts", "RDNA3Driver.kext")
CONFIG_PATH = os.path.join(PROJECT_ROOT, "hardware", "efi_firebat_u6c_simplify", "EFI", "OC", "config.plist")
OCVALIDATE_PATH = "/tmp/opencore_extracted/Utilities/ocvalidate/ocvalidate.linux"


def align_up(val, align):
    return (val + align - 1) & ~(align - 1)


def run_cmd(cmd, check=True):
    print("RUN:", " ".join(cmd))
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0 and check:
        print("STDOUT:", res.stdout)
        print("STDERR:", res.stderr)
        raise RuntimeError(f"Command failed with code {res.returncode}")
    return res


def pack_macho_kext(obj_path, out_path):
    """
    Packs a single-translation-unit MH_OBJECT (.o) compiled with -fapple-kext -mkernel
    into an authentic MH_KEXT_BUNDLE (0x0B) with classic LC_DYSYMTAB extrel and locrel
    relocation tables compatible with OpenCore OcAppleKernelLib / KXLD.
    """
    with open(obj_path, "rb") as f:
        data = f.read()

    off = 32
    ncmds = struct.unpack_from("<I", data, 16)[0]
    old_sects = []
    symoff = nsyms = stroff = strsize = 0
    dysym = None

    for _ in range(ncmds):
        c, csize = struct.unpack_from("<II", data, off)
        if c == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            for s in range(nsects):
                soff = off + 72 + s * 80
                sectname = data[soff:soff + 16].rstrip(b"\x00").decode()
                sseg = data[soff + 16:soff + 32].rstrip(b"\x00").decode()
                saddr, ssize, sfileoff, salign, sreloff, snreloc, sflags, r1, r2, r3 = struct.unpack_from(
                    "<QQIIIIIIII", data, soff + 32
                )
                raw = bytearray(data[sfileoff:sfileoff + ssize]) if (sflags & 0xFF) != 1 else bytearray(ssize)
                relocs = []
                for r in range(snreloc):
                    r_addr, r_info = struct.unpack_from("<iI", data, sreloff + r * 8)
                    relocs.append((r_addr, r_info))
                old_sects.append({
                    "idx1": s + 1,
                    "seg": sseg,
                    "sect": sectname,
                    "old_addr": saddr,
                    "size": ssize,
                    "align": salign,
                    "flags": sflags,
                    "raw": raw,
                    "relocs": relocs,
                })
        elif c == 0x02:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
        elif c == 0x0B:  # LC_DYSYMTAB
            dysym = struct.unpack_from("<18I", data, off + 8)
        off += csize

    old_syms = []
    for i in range(nsyms):
        strx, stype, ssect, sdesc, sval = struct.unpack_from("<IBBHQ", data, symoff + i * 16)
        end = data.find(b"\x00", stroff + strx)
        name = data[stroff + strx:end].decode()
        old_syms.append({
            "name": name,
            "type": stype,
            "sect": ssect,
            "desc": sdesc,
            "old_val": sval,
        })

    # Collect unique symbols referenced via X86_64_RELOC_GOT_LOAD (type 3)
    got_sym_indices = []
    for s in old_sects:
        for r_addr, r_info in s["relocs"]:
            r_type = (r_info >> 28) & 0xF
            r_sym = r_info & 0xFFFFFF
            if r_type == 3 and r_sym not in got_sym_indices:
                got_sym_indices.append(r_sym)

    text_sects = [s for s in old_sects if s["seg"] == "__TEXT"]
    data_regular = [s for s in old_sects if s["seg"] == "__DATA" and (s["flags"] & 0xFF) != 1]
    data_zerofill = [s for s in old_sects if s["seg"] == "__DATA" and (s["flags"] & 0xFF) == 1]

    got_sect = {
        "idx1": 0,
        "seg": "__DATA",
        "sect": "__got",
        "old_addr": 0,
        "size": len(got_sym_indices) * 8,
        "align": 3,
        "flags": 0,
        "raw": bytearray(len(got_sym_indices) * 8),
        "relocs": [],
    }
    data_sects = [got_sect] + data_regular + data_zerofill

    cur_addr = 0x800
    new_idx1 = 1
    old_to_new_sect = {}

    for s in text_sects:
        cur_addr = align_up(cur_addr, 1 << s["align"])
        s["new_addr"] = cur_addr
        s["new_idx1"] = new_idx1
        old_to_new_sect[s["idx1"]] = s
        cur_addr += s["size"]
        new_idx1 += 1

    text_vmsize = align_up(cur_addr, 0x1000)
    data_vmaddr = text_vmsize
    cur_addr = data_vmaddr

    for s in data_sects:
        cur_addr = align_up(cur_addr, 1 << s["align"])
        s["new_addr"] = cur_addr
        s["new_idx1"] = new_idx1
        if s["idx1"] != 0:
            old_to_new_sect[s["idx1"]] = s
        cur_addr += s["size"]
        new_idx1 += 1

    data_vmsize = align_up(cur_addr - data_vmaddr, 0x1000)
    linkedit_vmaddr = data_vmaddr + data_vmsize

    for sym in old_syms:
        if sym["sect"] != 0:
            target_s = old_to_new_sect[sym["sect"]]
            sym["new_val"] = target_s["new_addr"] + (sym["old_val"] - target_s["old_addr"])
            sym["new_sect"] = target_s["new_idx1"]
        else:
            sym["new_val"] = 0
            sym["new_sect"] = 0

    extdef_range = range(dysym[2], dysym[2] + dysym[3])
    undef_range = range(dysym[4], dysym[4] + dysym[5])

    out_syms = []
    old_to_new_sym = {}
    for old_i in extdef_range:
        old_to_new_sym[old_i] = len(out_syms)
        out_syms.append(old_syms[old_i])
    nextdefsym = len(out_syms)

    for old_i in undef_range:
        old_to_new_sym[old_i] = len(out_syms)
        out_syms.append(old_syms[old_i])
    nundefsym = len(out_syms) - nextdefsym

    ext_relocs = []
    loc_relocs = []

    for g_idx, old_sym_i in enumerate(got_sym_indices):
        r_addr = got_sect["new_addr"] + g_idx * 8
        new_sym_i = old_to_new_sym[old_sym_i]
        r_info = (new_sym_i & 0xFFFFFF) | (0 << 24) | (3 << 25) | (1 << 27) | (0 << 28)
        ext_relocs.append((r_addr, r_info))

    for s in text_sects + data_regular:
        relocs = s["relocs"]
        r_idx = 0
        while r_idx < len(relocs):
            r_addr, r_info = relocs[r_idx]
            r_sym = r_info & 0xFFFFFF
            r_pcrel = (r_info >> 24) & 1
            r_len = (r_info >> 25) & 3
            r_ext = (r_info >> 27) & 1
            r_type = (r_info >> 28) & 0xF
            abs_addr = s["new_addr"] + r_addr

            if r_type == 5:  # X86_64_RELOC_SUBTRACTOR paired with X86_64_RELOC_UNSIGNED
                r_addr2, r_info2 = relocs[r_idx + 1]
                r_sym2 = r_info2 & 0xFFFFFF
                val32 = struct.unpack_from("<i", s["raw"], r_addr)[0]
                diff32 = old_syms[r_sym2]["new_val"] - old_syms[r_sym]["new_val"] + val32
                struct.pack_into("<i", s["raw"], r_addr, diff32)
                r_idx += 2
                continue

            if s["seg"] == "__TEXT":
                val32 = struct.unpack_from("<i", s["raw"], r_addr)[0]
                if r_type in (1, 2):  # Signed or Branch
                    if r_ext == 0:
                        target_in_old = (s["old_addr"] + r_addr + 4) + val32
                        t_sect = old_to_new_sect[r_sym]
                        target_new = t_sect["new_addr"] + (target_in_old - t_sect["old_addr"])
                        disp32 = target_new - (abs_addr + 4)
                        struct.pack_into("<i", s["raw"], r_addr, disp32)
                    else:
                        sym = old_syms[r_sym]
                        if sym["sect"] != 0:
                            target_new = sym["new_val"] + val32
                            disp32 = target_new - (abs_addr + 4)
                            struct.pack_into("<i", s["raw"], r_addr, disp32)
                        else:
                            assert r_type == 2, f"Unexpected undefined __TEXT reloc type {r_type}"
                            new_sym_i = old_to_new_sym[r_sym]
                            new_info = (new_sym_i & 0xFFFFFF) | (1 << 24) | (2 << 25) | (1 << 27) | (2 << 28)
                            ext_relocs.append((abs_addr, new_info))
                elif r_type == 3:  # X86_64_RELOC_GOT_LOAD
                    g_idx = got_sym_indices.index(r_sym)
                    got_addr = got_sect["new_addr"] + g_idx * 8
                    disp32 = got_addr - (abs_addr + 4)
                    struct.pack_into("<i", s["raw"], r_addr, disp32)
                else:
                    raise RuntimeError(f"Unexpected __TEXT reloc type {r_type}")
            else:
                assert r_type == 0 and r_len == 3 and r_pcrel == 0
                val64 = struct.unpack_from("<Q", s["raw"], r_addr)[0]
                if r_ext == 1 and old_syms[r_sym]["sect"] == 0:
                    new_sym_i = old_to_new_sym[r_sym]
                    new_info = (new_sym_i & 0xFFFFFF) | (0 << 24) | (3 << 25) | (1 << 27) | (0 << 28)
                    ext_relocs.append((abs_addr, new_info))
                else:
                    if r_ext == 1:
                        target_new = old_syms[r_sym]["new_val"] + val64
                        target_sect_idx1 = old_syms[r_sym]["new_sect"]
                    else:
                        t_sect = old_to_new_sect[r_sym]
                        target_new = t_sect["new_addr"] + (val64 - t_sect["old_addr"])
                        target_sect_idx1 = t_sect["new_idx1"]
                    struct.pack_into("<Q", s["raw"], r_addr, target_new)
                    new_info = (target_sect_idx1 & 0xFFFFFF) | (0 << 24) | (3 << 25) | (0 << 27) | (0 << 28)
                    loc_relocs.append((abs_addr, new_info))
            r_idx += 1

    loc_relocs.sort(key=lambda x: x[0])
    ext_relocs.sort(key=lambda x: x[0])

    locreloff = linkedit_vmaddr
    nlocrel = len(loc_relocs)
    locrel_bytes = bytearray()
    for r_addr, r_info in loc_relocs:
        locrel_bytes += struct.pack("<iI", r_addr, r_info)

    symoff = locreloff + len(locrel_bytes)
    nsyms = len(out_syms)
    strtab_bytes = bytearray(b"\x00")
    symtab_bytes = bytearray()
    for sym in out_syms:
        strx = len(strtab_bytes)
        strtab_bytes += sym["name"].encode() + b"\x00"
        symtab_bytes += struct.pack("<IBBHQ", strx, sym["type"], sym["new_sect"], sym["desc"], sym["new_val"])

    extreloff = symoff + len(symtab_bytes)
    nextrel = len(ext_relocs)
    extrel_bytes = bytearray()
    for r_addr, r_info in ext_relocs:
        extrel_bytes += struct.pack("<iI", r_addr, r_info)

    stroff = extreloff + len(extrel_bytes)
    while len(strtab_bytes) % 16 != 0:
        strtab_bytes += b"\x00"
    strsize = len(strtab_bytes)

    linkedit_raw = locrel_bytes + symtab_bytes + extrel_bytes + strtab_bytes
    linkedit_filesize = len(linkedit_raw)
    linkedit_vmsize = align_up(linkedit_filesize, 0x1000)

    def pack_segment_cmd(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, sects):
        cmdsize = 72 + len(sects) * 80
        hdr = struct.pack(
            "<II16sQQQQIIII",
            0x19, cmdsize, name.encode().ljust(16, b"\x00"),
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, len(sects), 0
        )
        s_bytes = bytearray()
        for s in sects:
            soff = 0 if (s["flags"] & 0xFF) == 1 else s["new_addr"]
            s_bytes += struct.pack(
                "<16s16sQQIIIIIIII",
                s["sect"].encode().ljust(16, b"\x00"),
                s["seg"].encode().ljust(16, b"\x00"),
                s["new_addr"], s["size"], soff, s["align"],
                0, 0, s["flags"], 0, 0, 0
            )
        return hdr + s_bytes

    lc_text = pack_segment_cmd("__TEXT", 0, text_vmsize, 0, text_vmsize, 5, 5, text_sects)
    lc_data = pack_segment_cmd("__DATA", data_vmaddr, data_vmsize, data_vmaddr, data_vmsize, 3, 3, data_sects)
    lc_linkedit = pack_segment_cmd("__LINKEDIT", linkedit_vmaddr, linkedit_vmsize, linkedit_vmaddr, linkedit_filesize, 1, 1, [])
    lc_symtab = struct.pack("<IIIIII", 0x02, 24, symoff, nsyms, stroff, strsize)
    lc_dysymtab = struct.pack(
        "<20I", 0x0B, 80,
        0, 0,
        0, nextdefsym,
        nextdefsym, nundefsym,
        0, 0, 0, 0, 0, 0, 0, 0,
        extreloff, nextrel,
        locreloff, nlocrel
    )
    lc_uuid = struct.pack("<II16s", 0x1B, 24, uuid.uuid4().bytes)

    cmds = lc_text + lc_data + lc_linkedit + lc_symtab + lc_dysymtab + lc_uuid
    mach_hdr = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x01000007, 0x00000003, 0x0B, 6, len(cmds), 0x85, 0)

    out_buf = bytearray(linkedit_vmaddr)
    out_buf[0:32 + len(cmds)] = mach_hdr + cmds

    for s in text_sects + data_regular:
        a = s["new_addr"]
        out_buf[a:a + s["size"]] = s["raw"]

    out_buf += linkedit_raw

    with open(out_path, "wb") as f:
        f.write(out_buf)
    os.chmod(out_path, 0o755)
    print(f"Packed MH_KEXT_BUNDLE: {len(out_buf)} bytes (extrel={nextrel}, locrel={nlocrel}, syms={nsyms})")


def build_kext():
    print("=== Building RDNA3Driver.kext for macOS Sonoma (14.x) and Tahoe (15.x+) ===")
    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(os.path.join(KEXT_BUNDLE_DIR, "Contents", "MacOS"), exist_ok=True)

    unity_src = os.path.join(BUILD_DIR, "kext_unity.cpp")
    unity_code = """// Auto-generated unity translation unit for RDNA3Driver.kext
#include "amd/rdna3_dcn314_scanout.cpp"
#include "amd/rdna3_ip_discovery.cpp"
#include "amd/rdna3_rebar_uma.cpp"
#include "amd/rdna3_ip_blocks.cpp"
#include "kext/RDNA3Framebuffer.cpp"
#include "kext/RDNA3UserClient.cpp"
#include "kext/RDNA3Driver.cpp"
extern "C" {
#include "kext/kmod_info.c"
}
"""
    with open(unity_src, "w") as f:
        f.write(unity_code)

    unity_obj = os.path.join(BUILD_DIR, "kext_unity.o")
    cxx_cmd = [
        "clang++",
        "-arch", "x86_64",
        "-target", "x86_64-apple-macos14.0",
        "-mmacosx-version-min=14.0",
        "-I" + SDK_HEADERS,
        "-I" + os.path.join(PROJECT_ROOT, "src"),
        "-I" + os.path.join(PROJECT_ROOT, "src", "kext"),
        "-DKERNEL", "-DKERNEL_PRIVATE", "-DDRIVER_PRIVATE", "-DAPPLE", "-DNeXT",
        "-D_FORTIFY_SOURCE=0",
        "-nostdinc",
        "-fno-builtin",
        "-fno-common",
        "-fno-stack-protector",
        "-mkernel",
        "-fapple-kext",
        "-std=c++17",
        "-fno-exceptions",
        "-fno-rtti",
        "-fcheck-new",
        "-fno-c++-static-destructors",
        "-Wall",
        "-Os",
        "-c", unity_src,
        "-o", unity_obj,
    ]
    run_cmd(cxx_cmd)

    kext_exec = os.path.join(KEXT_BUNDLE_DIR, "Contents", "MacOS", "RDNA3Driver")
    pack_macho_kext(unity_obj, kext_exec)

    src_plist = os.path.join(PROJECT_ROOT, "src", "kext", "Info.plist")
    dst_plist = os.path.join(KEXT_BUNDLE_DIR, "Contents", "Info.plist")
    shutil.copyfile(src_plist, dst_plist)

    print(f"Kext bundle successfully created at {KEXT_BUNDLE_DIR}")

    if os.path.exists(DEST_KEXT_DIR):
        shutil.rmtree(DEST_KEXT_DIR)
    shutil.copytree(KEXT_BUNDLE_DIR, DEST_KEXT_DIR)
    print(f"Deployed kext to {DEST_KEXT_DIR}")

    with open(CONFIG_PATH, "rb") as f:
        pl = plistlib.load(f)

    kext_entries = pl.get("Kernel", {}).get("Add", [])
    already_present = any("RDNA3Driver.kext" in k.get("BundlePath", "") for k in kext_entries)
    if not already_present:
        print("Registering RDNA3Driver.kext in config.plist Kernel -> Add")
        new_entry = {
            "Arch": "x86_64",
            "BundlePath": "RDNA3Driver.kext",
            "Comment": "Native AMD RDNA 3 (Radeon 780M / GFX1103) Graphics Driver",
            "Enabled": True,
            "ExecutablePath": "Contents/MacOS/RDNA3Driver",
            "MaxKernel": "",
            "MinKernel": "22.0.0",
            "PlistPath": "Contents/Info.plist"
        }
        kext_entries.insert(2, new_entry)
        pl["Kernel"]["Add"] = kext_entries

        with open(CONFIG_PATH, "wb") as f:
            plistlib.dump(pl, f)
        print("config.plist updated with RDNA3Driver.kext")
    else:
        print("RDNA3Driver.kext already registered in config.plist")

    if os.path.exists(OCVALIDATE_PATH):
        res = run_cmd([OCVALIDATE_PATH, CONFIG_PATH])
        print("ocvalidate output:\n", res.stdout)
    else:
        print("Warning: ocvalidate not found at", OCVALIDATE_PATH)

    print("=== Build and deployment completed successfully! ===")


if __name__ == "__main__":
    build_kext()
