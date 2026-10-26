#!/usr/bin/env python3
"""Compares the CP registers Linux amdgpu leaves (strix_regdump.py) with nullarc's (the `cpscan before ...` lines of a
try log, which lists only the non-zero ones). Prints the differences, grouped: configuration registers first, then
queue/pointer/status ones that differ by nature.

usage: compare_cp_regs.py <strix_linux_regs.txt> <nullarc_tryN.txt>
"""
import re
import sys

# registers whose value is per-run state rather than configuration
STATE = re.compile(r'RPTR|WPTR|STAT|BUSY|DUMP|INSTR_PNTR|PNTR|_LO$|_HI$|FENCE|TIME|CYCLE|COUNT|OFFSET|HEADER|PERFCOUNTER|'
                   r'EOP_DONE|IB[12]|ROQ|MEQ|STQ|DMA_|SCRATCH|ECC|ERROR|INT_STAT|LAST|PROGRESS|_CNTR|TAGS|DOORBELL')

lin, mac = {}, {}
for line in open(sys.argv[1]):
    m = re.match(r'(\w+) \(0x([0-9a-f]+)\) = 0x([0-9a-f]+)', line)
    if m:
        lin[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
for line in open(sys.argv[2]):
    m = re.match(r'cpscan before[^:]*: (\w+) \(0x([0-9a-f]+)\) = 0x([0-9a-f]+)', line)
    if m:
        mac[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
if not mac:
    sys.exit("no 'cpscan before' lines in the try log")

cfg, st = [], []
for name, (reg, lv) in sorted(lin.items(), key=lambda x: x[1][0]):
    mv = mac.get(name, (reg, 0))[1]
    if lv != mv:
        (st if STATE.search(name) else cfg).append(f"  {name:44} 0x{reg:05x}  linux 0x{lv:08x}  nullarc 0x{mv:08x}")
print(f"{len(lin)} registers compared, {len(cfg)} configuration and {len(st)} state differences\n")
print("== configuration registers that differ")
print("\n".join(cfg) or "  (none)")
print("\n== state / pointer registers that differ")
print("\n".join(st) or "  (none)")
