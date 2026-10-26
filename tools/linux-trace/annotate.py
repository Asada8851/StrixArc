#!/usr/bin/env python3
"""Names the registers of an amdgpu MMIO trace (tools/linux-trace/trace_strix.sh) on the 880M.

usage: annotate.py <strix_regtrace.txt[.zst]> <dir with the Linux *_offset.h headers> [--writes] [--block GC,...]
prints: time  r/w  absolute-dword  value  BLOCK:REGNAME
"""
import re
import subprocess
import sys
from collections import defaultdict

# IP discovery of the 880M (tools/parse_discovery.py ~/strix_ip_discovery.bin): base index -> absolute dword base
IPS = {
    "GC":     ("gc_11_5_0_offset.h",     [0x1260, 0xA000, 0x2402C00, 0x3C02C000]),
    "MMHUB":  ("mmhub_3_3_0_offset.h",   [0x13200, 0x1A000, 0x2408800, 0x3C088000]),
    "NBIO":   ("nbio_7_11_0_offset.h",   [0x0, 0x14, 0xD20, 0x10400, 0x241B000, 0x4040000, 0x3C1B0000]),
    "MP":     ("mp_14_0_0_offset.h",     [0x16000, 0xDC0000, 0xE00000, 0xE40000, 0x243FC00, 0x3C3FC000]),
    "HDP":    ("hdp_6_0_0_offset.h",     [0xF20, 0x240A400, 0x3C0A4000]),
    "OSSSYS": ("osssys_6_1_0_offset.h",  [0x10A0, 0x240A000, 0x3C0A0000]),
    "SMUIO":  ("smuio_14_0_2_offset.h",  [0x16800, 0x16A00, 0x440000, 0x2401000, 0x3C010000]),
    "ATHUB":  ("athub_3_0_0_offset.h",   [0xC00, 0x13300, 0x2408C00, 0x3C08C000]),
    "DCN":    ("dcn_3_5_0_offset.h",     [0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00, 0x3C03C000]),
}


def load_names(hdir):
    names = defaultdict(list)
    for blk, (hdr, bases) in IPS.items():
        try:
            text = open(f"{hdir}/{hdr}").read()
        except OSError:
            continue
        off = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'#define reg(\w+?)\s+(0x[0-9a-fA-F]+)\s*$', text, re.M)}
        idx = {m.group(1): int(m.group(2)) for m in re.finditer(r'#define reg(\w+)_BASE_IDX\s+(\d+)', text)}
        for n, o in off.items():
            i = idx.get(n)
            if i is None or i >= len(bases):
                continue
            names[bases[i] + o].append(f"{blk}:{n}")
    return names


def events(path):
    if path.endswith(".zst"):
        src = subprocess.run(["zstd", "-dc", path], capture_output=True, text=True, check=True).stdout.splitlines()
    else:
        src = open(path).read().splitlines()
    pat = re.compile(r'\s(\d+\.\d+): amdgpu_device_(r|w)reg: 0x[0-9a-f]+, 0x([0-9a-f]+), 0x([0-9a-f]+)')
    for line in src:
        m = pat.search(line)
        if m:
            yield float(m.group(1)), m.group(2), int(m.group(3), 16), int(m.group(4), 16)


def main():
    path, hdir = sys.argv[1], sys.argv[2]
    writes_only = "--writes" in sys.argv
    blocks = None
    if "--block" in sys.argv:
        blocks = set(sys.argv[sys.argv.index("--block") + 1].split(","))
    names = load_names(hdir)
    for t, rw, reg, val in events(path):
        if writes_only and rw != "w":
            continue
        n = names.get(reg, ["?"])
        if blocks and not any(x.split(":")[0] in blocks for x in n):
            continue
        print(f"{t:12.6f} {rw} 0x{reg:06x} 0x{val:08x} {'/'.join(n[:2])}")


if __name__ == "__main__":
    main()
