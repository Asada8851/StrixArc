#!/usr/bin/env python3
"""
tools/parse_discovery.py — decode an AMD IP discovery binary (as read by PhxProbe.kext).

Layout follows Linux drivers/gpu/drm/amd/include/discovery.h (binary_header ->
ip_discovery_header -> die_header -> ip/ip_v3/ip_v4 entries).
Input: a raw .bin file, or the hex string from `ioreg -l` (phx-discovery-bin = <...>).
"""

import re
import struct
import sys

BINARY_SIGNATURE = 0x28211407
DISCOVERY_TABLE_SIGNATURE = 0x53445049

HWID_NAMES = {
    1: "MP1(SMU)", 2: "MP2", 3: "THM", 4: "SMUIO", 5: "FUSE", 6: "CLKA", 10: "PWR", 11: "GC",
    12: "VCN", 13: "AUDIO_AZ", 14: "ACP", 15: "DCI", 16: "DCO", 17: "XDMA", 18: "DCEAZ",
    19: "SDPMUX", 20: "NTB", 21: "VPE", 22: "UMSCH", 24: "IOHC", 28: "L2IMU", 32: "VCE",
    34: "MMHUB", 35: "ATHUB", 36: "DBGU_NBIO", 37: "DFX", 38: "DBGU0", 39: "DBGU1",
    40: "OSSSYS", 41: "HDP", 42: "SDMA0", 43: "SDMA1", 44: "ISP", 45: "DBGU_IO", 46: "DF",
    47: "CLKB", 48: "FCH", 49: "DFX_DAP", 68: "SDMA2", 69: "SDMA3", 70: "PCIE", 80: "PCS",
    89: "DDCL", 90: "SST", 91: "LSDMA", 100: "IOAGR", 108: "NBIF", 124: "IOAPIC",
    128: "SYSTEMHUB", 144: "NTBCCP", 150: "UMC", 168: "SATA", 170: "USB", 176: "CCXSEC",
    200: "XGMI", 216: "XGBE", 255: "MP0(PSP)", 271: "DMU", 272: "DIO", 274: "DAZ", 294: "ATU",
}


def load(path):
    raw = open(path, "rb").read()
    text = raw.decode("latin-1")
    m = re.search(r"<([0-9a-fA-F\s]+)>", text)
    if m and raw[:4] != struct.pack("<I", BINARY_SIGNATURE):
        return bytes.fromhex(re.sub(r"\s", "", m.group(1)))
    return raw


def ip_blocks(data, verbose=False):
    """Yields (name, hw_id, instance, (major, minor, rev), bases) for every IP block of every die."""
    sig, vmaj, vmin, csum, size = struct.unpack_from("<IHHHH", data, 0)
    if sig != BINARY_SIGNATURE:
        raise SystemExit(f"bad binary signature 0x{sig:08x}")
    if verbose:
        print(f"binary header v{vmaj}.{vmin} size={size}")
    if vmaj >= 2:
        num_tables, = struct.unpack_from("<H", data, 12)
        tables_off = 16
    else:
        num_tables, tables_off = 6, 12
    tables = [struct.unpack_from("<HHHH", data, tables_off + 8 * i) for i in range(num_tables)]
    ipd_off = tables[0][0]

    isig, iver, isize, iid, ndies = struct.unpack_from("<IHHIH", data, ipd_off)
    if isig != DISCOVERY_TABLE_SIGNATURE:
        raise SystemExit(f"bad IP discovery signature 0x{isig:08x}")
    dies = [struct.unpack_from("<HH", data, ipd_off + 14 + 4 * i) for i in range(ndies)]
    flags = data[ipd_off + 14 + 4 * 16] if iver >= 4 else 0
    addr64 = bool(flags & 1)
    if verbose:
        print(f"ip discovery v{iver}, dies={ndies}, 64-bit base addresses={addr64}")

    for die_id, die_off in dies:
        did, nips = struct.unpack_from("<HH", data, die_off)
        if verbose:
            print(f"\n die {did}: {nips} IP blocks")
        pos = die_off + 4
        for _ in range(nips):
            hw_id, inst, nbase, major, minor, rev, extra = struct.unpack_from("<HBBBBBB", data, pos)
            pos += 8
            fmt, step = ("<Q", 8) if addr64 else ("<I", 4)
            bases = [struct.unpack_from(fmt, data, pos + step * k)[0] for k in range(nbase)]
            pos += step * nbase
            sub = (extra & 0xF) if iver >= 3 else 0
            yield HWID_NAMES.get(hw_id, f"hwid{hw_id}"), hw_id, inst, (major, minor, rev), bases, sub


def parse(data):
    for name, _, inst, (major, minor, rev), bases, sub in ip_blocks(data, verbose=True):
        base_s = " ".join(f"0x{b:x}" for b in bases)
        print(f"  {name:12} inst {inst}  v{major}.{minor}.{rev}"
              f"{'.' + str(sub) if sub else ''}  bases: {base_s}")


if __name__ == "__main__":
    parse(load(sys.argv[1]))
