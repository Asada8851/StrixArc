#!/usr/bin/env python3
"""Check the register table in src/amd/rdna3_chip.h against a Linux kernel tree.

    tools/check_chip_regs.py /path/to/linux

Every GCVM/OTG field of each chip profile is looked up by name in the matching asic_reg header
(gc_11_0_0 / dcn_3_1_4 for Phoenix, gc_11_5_0 / dcn_3_5_0 for Strix Point); a mismatch is a bug in the table.
Only the register offsets are checked: the segment bases (GC 0x1260, DCN 0x34C0) come from Phoenix's IP
discovery and still need a Strix Point discovery dump to confirm.
"""
import re
import sys
from pathlib import Path

# field order of struct rdna3_gcvm_regs -> Linux register name
GCVM = [
    "GCVM_L2_CNTL", "GCVM_L2_CNTL2", "GCVM_L2_CNTL3", "GCVM_L2_CNTL4", "GCVM_L2_CNTL5",
    "GCVM_L2_PROTECTION_FAULT_CNTL", "GCVM_L2_PROTECTION_FAULT_CNTL2", "GCVM_L2_PROTECTION_FAULT_STATUS",
    "GCVM_L2_PROTECTION_FAULT_ADDR_LO32", "GCVM_L2_PROTECTION_FAULT_ADDR_HI32",
    "GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32", "GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32",
    "GCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32", "GCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32",
    "GCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32",
    "GCMC_VM_AGP_TOP", "GCMC_VM_AGP_BOT", "GCMC_VM_AGP_BASE",
    "GCMC_VM_SYSTEM_APERTURE_LOW_ADDR", "GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR",
    "GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB", "GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB",
    "GCMC_VM_MX_L1_TLB_CNTL",
    "GCMC_VM_FB_LOCATION_BASE", "GCMC_VM_FB_LOCATION_TOP",
    "GCVM_CONTEXT0_CNTL",
    "GCVM_INVALIDATE_ENG0_REQ", "GCVM_INVALIDATE_ENG0_ACK", "GCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32",
    "GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32", "GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32",
    "GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32",
]
OTG = ["OTG0_OTG_H_TOTAL", "OTG0_OTG_V_TOTAL", "OTG0_OTG_CONTROL", "OTG0_OTG_STATUS_FRAME_COUNT"]
CHIPS = {"rdna3_chip_phoenix": ("gc/gc_11_0_0_offset.h", "dcn/dcn_3_1_4_offset.h", "mmhub/mmhub_3_0_1_offset.h"),
         "rdna3_chip_strix_point": ("gc/gc_11_5_0_offset.h", "dcn/dcn_3_5_0_offset.h", "mmhub/mmhub_3_3_0_offset.h")}
MMHUB = ["MMMC_VM_FB_LOCATION_BASE", "MMMC_VM_FB_LOCATION_TOP", "MMMC_VM_FB_OFFSET"]
MMHUB_SEG1 = 0x1A000
GC_SEG = [0x1260, 0xA000]
DCN_SEG2 = 0x34C0


def regs(path):
    text = path.read_text()
    out = {}
    for m in re.finditer(r"#define reg(\w+)\s+0x([0-9a-fA-F]+)\s*\n#define reg\1_BASE_IDX\s+(\d+)", text):
        out[m.group(1)] = (int(m.group(2), 16), int(m.group(3)))
    return out


def table(src, var):
    body = re.search(r"static const struct rdna3_chip " + var + r" = \{(.*?)\n\};", src, re.S).group(1)
    gcvm = [int(v, 16) for v in re.findall(r"RDNA3_GCVM_\((0x[0-9a-fA-F]+)\)", body)]
    otg = [int(v, 16) for v in re.findall(r"RDNA3_OTG_\((0x[0-9a-fA-F]+)\)", body)]
    return gcvm, otg


def main():
    linux = Path(sys.argv[1] if len(sys.argv) > 1 else "../linux")
    inc = linux / "drivers/gpu/drm/amd/include/asic_reg"
    src = (Path(__file__).resolve().parent.parent / "src/amd/rdna3_chip.h").read_text()
    bad = 0
    for var, (gch, dcnh, mmh) in CHIPS.items():
        gc, dcn, mm = regs(inc / gch), regs(inc / dcnh), regs(inc / mmh)
        body = re.search(r"static const struct rdna3_chip " + var + r" = \{(.*?)\n\};", src, re.S).group(1)
        have_mm = [int(v, 16) for v in re.findall(r"RDNA3_MMHUB_SEG1 \+ (0x[0-9a-fA-F]+)u", body)]
        for name, have in zip(MMHUB, have_mm):
            off, seg = mm[name]
            if seg != 1 or off != have:
                print(f"{var}: {name} is seg{seg} 0x{off:x} in {mmh}, table has 0x{have:x}")
                bad += 1
        assert len(have_mm) == len(MMHUB), f"{var}: MMHUB field count"
        gcvm, otg = table(src, var)
        assert len(gcvm) == len(GCVM) and len(otg) == len(OTG), f"{var}: field count"
        for name, have in zip(GCVM, gcvm):
            off, seg = gc[name]
            if seg != 0 or off != have:
                print(f"{var}: {name} is seg{seg} 0x{off:x} in {gch}, table has 0x{have:x}")
                bad += 1
        for name, have in zip(OTG, otg):
            off, seg = dcn[name]
            if seg != 2 or off != have:
                print(f"{var}: {name} is seg{seg} 0x{off:x} in {dcnh}, table has 0x{have:x}")
                bad += 1
        print(f"{var}: {len(GCVM)} GCVM + {len(OTG)} OTG + {len(MMHUB)} MMHUB registers checked against {gch},"
              f" {dcnh}, {mmh}")
    bad += check_translate(inc, src)
    sys.exit(1 if bad else 0)


SPAN = [1] * 25 + [16, 18, 18, 36, 32, 32, 32]


def check_translate(inc, src):
    """Re-implements rdna3_gcvm_translate() and runs it over every GC 11.0 register in the Phoenix GCVM window:
    each one must either land on the GC 11.5 register of the same name or be refused."""
    first = int(re.search(r"RDNA3_PHX_GCVM_FIRST \(RDNA3_GC_SEG0 \+ (0x[0-9a-fA-F]+)u\)", src).group(1), 16)
    last = int(re.search(r"RDNA3_PHX_GCVM_LAST\s+\(RDNA3_GC_SEG0 \+ (0x[0-9a-fA-F]+)u\)", src).group(1), 16)
    span = [int(v) for v in re.search(r"rdna3_gcvm_span\[\] = \{(.*?)\};", src, re.S).group(1).replace(",", " ").split()]
    assert span == SPAN, "rdna3_gcvm_span changed: update SPAN"
    phx, strix = table(src, "rdna3_chip_phoenix")[0], table(src, "rdna3_chip_strix_point")[0]
    a, b = regs(inc / "gc/gc_11_0_0_offset.h"), regs(inc / "gc/gc_11_5_0_offset.h")
    by_off = {}
    for name, (off, seg) in a.items():
        if seg == 0:
            by_off.setdefault(off, []).append(name)
    bad = mapped = refused = 0
    for off in range(first, last + 1):
        hit = None
        for f, t, n in zip(phx, strix, span):
            if f <= off < f + n:
                hit = t + (off - f)
                break
        for name in by_off.get(off, []):
            if hit is None:
                refused += 1
                continue
            if name not in b or b[name][1] != 0:
                print(f"translate: {name} (0x{off:x}) maps to 0x{hit:x} but has no seg0 GC 11.5 register")
                bad += 1
            elif b[name][0] != hit:
                print(f"translate: {name} 0x{off:x} -> 0x{hit:x}, GC 11.5 has 0x{b[name][0]:x}")
                bad += 1
            else:
                mapped += 1
    if last < max(f + n - 1 for f, n in zip(phx, span)):
        print("translate: the window ends before the last table range")
        bad += 1
    print(f"translate: {mapped} GC 11.0 registers map to their GC 11.5 names, {refused} refused, {bad} wrong")
    return bad


if __name__ == "__main__":
    main()
