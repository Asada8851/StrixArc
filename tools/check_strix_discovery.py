#!/usr/bin/env python3
"""Check a Strix Point IP discovery dump against what nullarc assumes (src/amd/rdna3_chip.h, PhxNativeABI.h).

On the 880M/890M machine, under any Linux with amdgpu loaded:
    sudo cat /sys/kernel/debug/dri/0/amdgpu_discovery > strix_ip_discovery.bin
then, anywhere:
    tools/check_strix_discovery.py strix_ip_discovery.bin

Every absolute register address nullarc uses is an IP base segment plus a register offset. The offsets come
from the Linux headers for each IP version; the segments were only ever seen on Phoenix. This prints, for each
one, the Strix value, whether it matches, and the number to put in the table when it does not.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_discovery import ip_blocks, load  # noqa: E402

# (what, IP name, base index, the segment nullarc uses, register offset in that segment, where the address lives)
# Base indices are the ones Linux uses for Strix Point's IP versions: psp_v13_0 (mp_13_0_2 headers) and
# smu_v14_0_0 address MP0/MP1 through base 0, where Phoenix's psp_v13_0_4 / smu_v13_0_4 use base 1.
CHECKS = [
    ("GC segment 0", "GC", 0, 0x1260, 0, "RDNA3_GC_SEG0; mesboot SEG0(); kext GCVM table"),
    ("GC segment 1", "GC", 1, 0xA000, 0, "RDNA3_GC_SEG1; mesboot SEG1()"),
    ("PSP mailbox (C2PMSG_64)", "MP0(PSP)", 0, 0x16000, 0x80, "PHX_PSP_C2PMSG_* in PhxNativeABI.h"),
    ("SMU mailbox (C2PMSG_66)", "MP1(SMU)", 0, 0x16000, 0x282, "PHX_SMU_*_REG in PhxNativeABI.h"),
    ("MMHUB FB location (MMMC_VM_FB_LOCATION_BASE)", "MMHUB", 1, 0x1A000, 0x9EC, "RDNA3_MMHUB_SEG1 (rdna3_chip.h)"),
    ("SMUIO GOLDEN_TSC_COUNT_UPPER", "SMUIO", 1, 0x16A00, 0x2D, "kGoldenTscUpper (kext), ACD_GOLDEN_TSC_UPPER_REG (Mesa 0004)"),
    ("DCN OTG0 (segment 2)", "DMU", 2, 0x34C0, 0x1B2A, "RDNA3_DCN_SEG2 (PhxAMDFB refresh rate)"),
    ("NBIO doorbell aperture enable (RCC_DOORBELL_APER_EN)", "NBIF", 2, 0xD20, 0xC0, "full.sh: phxctl wreg 0xDE0 1"),
]
EXPECT_VERSIONS = {"GC": (11, 5), "MP0(PSP)": (14, 0), "MP1(SMU)": (14, 0), "DMU": (3, 5), "SDMA0": (6, 1),
                   "MMHUB": (3, 3)}


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    blocks = {}
    for name, _, inst, ver, bases, _ in ip_blocks(load(sys.argv[1])):
        if inst == 0:
            blocks[name] = (ver, bases)
    bad = 0
    for name, (maj, mnr) in EXPECT_VERSIONS.items():
        ver = blocks.get(name, ((None,),))[0]
        ok = ver[:2] == (maj, mnr)
        print(f"{'ok  ' if ok else 'DIFF'} {name} v{'.'.join(map(str, ver))} (expected {maj}.{mnr}.x)")
        bad += not ok
    for what, ip, idx, seg, off, used in CHECKS:
        bases = blocks.get(ip, (None, []))[1]
        if idx >= len(bases):
            print(f"MISS {what}: {ip} has no base {idx} ({used})")
            bad += 1
            continue
        have = bases[idx]
        if have == seg:
            print(f"ok   {what}: {ip} base {idx} = 0x{have:x}, address 0x{seg + off:x}")
        else:
            print(f"DIFF {what}: {ip} base {idx} = 0x{have:x}, nullarc assumes 0x{seg:x}:"
                  f" use 0x{have + off:x} instead of 0x{seg + off:x} ({used})")
            bad += 1
    nbif = blocks.get("NBIF", (None, []))[1]
    if len(nbif) > 5:
        # nbio_7_11_0: BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL is segment 5 offset 0x8e4d (Phoenix's 7.7: segment 2, 0x12d).
        # PhxBringup reads registers through BAR5 directly (512 KiB, dwords below 0x20000); Linux reaches anything
        # above that through the NBIO PCIE_INDEX2/DATA2 pair, which nullarc does not implement yet.
        reg = nbif[5] + 0x8E4D
        if reg < 0x20000:
            print(f"set  rdna3_chip_strix_point.nbio_hdp_remap = 0x{reg:x}  (NBIF base 5 + 0x8e4d)")
        else:
            # not an error: PhxBringup falls back to the flush register itself (rdna3_chip.hdp_flush_direct)
            print(f"note HDP flush remap register at 0x{reg:x} is beyond BAR5: PhxBringup flushes through"
                  f" HDP_MEM_COHERENCY_FLUSH_CNTL at 0x{nbif[2] + 0xF7:x} instead")
    else:
        print("note NBIF base 5 unknown: PhxBringup flushes HDP through HDP_MEM_COHERENCY_FLUSH_CNTL (NBIF base 2 + 0xf7)")
    # MES SET_HW_RESOURCES wants the first five GC / MMHUB / OSSSYS bases (rdna3_chip.mes)
    mes = [blocks.get(n, (None, []))[1] for n in ("GC", "MMHUB", "OSSSYS")]
    if all(mes):
        five = lambda b: "{ " + ", ".join(f"0x{x:X}" for x in (list(b) + [0] * 5)[:5]) + " }"
        print("mes  rdna3_chip_strix_point.mes = { " + ", ".join(five(b) for b in mes) + " }")
    else:
        print("MISS GC/MMHUB/OSSSYS bases for MES SET_HW_RESOURCES")
        bad += 1
    print(f"\n{bad} assumption(s) to fix" if bad else "\nall of nullarc's Strix Point base assumptions hold")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
