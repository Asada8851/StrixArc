#!/usr/bin/env python3
"""Writes the register list dcn_probe.sh reads under macOS: the 880M's display pipe 0 (HUBP/HUBPREQ/HUBPRET, cursor,
DPP/DSCL, MPC/OPP, OTG0), all of DCHUBBUB (its FB/AGP/system apertures and VM: what memory the display can fetch),
DMCUB's state (is the display microcontroller running), the display clocks, and MMHUB's FB/AGP/system apertures.
Only registers that are plain to read: anything that is a data port, FIFO, index, LUT or RAM window is left out (a
read could advance it). Absolute dword addresses with the 880M's IP discovery bases; only those inside BAR5 (512 KiB).

usage: gen_dcn_probe_list.py <dcn_3_5_0_offset.h> <mmhub_3_3_0_offset.h> > dcn_regs.txt
"""
import re
import sys

DMU = [0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00]
MMHUB = [0x13200, 0x1A000, 0x2408800, 0x3C088000, 0x0]   # rdna3_chip_strix_point mes bases, checked against discovery
KEEP_DCN = re.compile(r'^(HUBP0_|HUBPREQ0_|HUBPRET0_|CURSOR0_0_|DCHUBBUB_|DCN_VM_|DPP_TOP0_|CNVC_CFG0_|CNVC_CUR0_|DSCL0_|'
                      r'MPCC0_|MPC_|OPP_PIPE0_|OPP_TOP_|FMT0_|OTG0_|OPTC_|ODM0_|DMCUB_|DP_DTO0_|OTG0_PIXEL|DENTIST_|'
                      r'DCCG_GATE|DCCG_GLOBAL|DPPCLK0_|DPPCLK_DTO|DCHUBP_CNTL|DC_MEM_GLOBAL|DCHVM_)')
KEEP_MMHUB = re.compile(r'^MMMC_VM_(FB_LOCATION|FB_OFFSET|AGP_|SYSTEM_APERTURE|MX_L1_TLB_CNTL|LOCAL_)')
SKIP = re.compile(r'(DATA|FIFO|INDEX|_LUT|RAM|COEF|CRC_DATA|_RDBACK|SEL_CNTL$)')


def regs(path, bases, keep):
    txt = open(path).read()
    for m in re.finditer(r'#define reg(\w+)\s+0x([0-9a-fA-F]+)\s*\n#define reg\1_BASE_IDX\s+(\d)', txt):
        name, off, idx = m.group(1), int(m.group(2), 16), int(m.group(3))
        a = bases[idx] + off
        if keep.search(name) and not SKIP.search(name) and a < 0x20000:
            yield a, name


def main():
    seen = set()
    for path, bases, keep in ((sys.argv[1], DMU, KEEP_DCN), (sys.argv[2], MMHUB, KEEP_MMHUB)):
        for a, n in regs(path, bases, keep):
            if a not in seen:
                seen.add(a)
                print('0x%05x %s' % (a, n))


if __name__ == '__main__':
    main()
