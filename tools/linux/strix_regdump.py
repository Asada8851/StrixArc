#!/usr/bin/env python3
"""Dumps the GC 11.5 CP registers (the table `mesboot cpscan` reads on macOS) from a running amdgpu, to compare the
CP state Linux sets up with nullarc's. Read-only apart from holding GFXOFF off while it reads (restored afterwards).

usage: sudo python3 tools/linux/strix_regdump.py [out file]     (default ~/strix_linux_regs.txt of the sudo user)
"""
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "..", "phxctl", "cp_regs_gc11_5.h")
EXTRA = [  # absolute dwords, GC 11.5 (gc_11_5_0_offset.h + segment base)
    (0x282C, "GCVM_L2_PROTECTION_FAULT_STATUS"), (0x282D, "GCVM_L2_PROTECTION_FAULT_ADDR_LO32"),
    (0x282E, "GCVM_L2_PROTECTION_FAULT_ADDR_HI32"), (0x2828, "GCVM_L2_PROTECTION_FAULT_CNTL"),
    (0x2829, "GCVM_L2_PROTECTION_FAULT_CNTL2"),
]


def debugfs_dir():
    for d in sorted(glob.glob("/sys/kernel/debug/dri/*")):
        name = os.path.join(d, "name")
        try:
            if "amdgpu" in open(name).read() and os.path.exists(os.path.join(d, "amdgpu_regs")):
                return d
        except OSError:
            pass
    sys.exit("no amdgpu debugfs directory (run as root, debugfs mounted?)")


def main():
    if os.geteuid() != 0:
        sys.exit("run with sudo")
    user = os.environ.get("SUDO_USER")
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(f"~{user}/strix_linux_regs.txt" if user else "~/strix_linux_regs.txt")
    regs = [(int(a, 16), n) for a, n in re.findall(r'\{0x([0-9a-f]+), "(\w+)"\}', open(TABLE).read())] + EXTRA
    d = debugfs_dir()
    gfxoff = os.path.join(d, "amdgpu_gfxoff")
    have_gfxoff = os.path.exists(gfxoff)
    if have_gfxoff:   # 0 = keep GFX powered while we read
        with open(gfxoff, "wb") as f:
            f.write(struct.pack("<I", 0))
    try:
        fd = os.open(os.path.join(d, "amdgpu_regs"), os.O_RDONLY)
        lines = []
        for reg, name in regs:
            v = struct.unpack("<I", os.pread(fd, 4, reg * 4))[0]
            lines.append(f"{name} (0x{reg:05x}) = 0x{v:08x}")
        os.close(fd)
    finally:
        if have_gfxoff:
            with open(gfxoff, "wb") as f:
                f.write(struct.pack("<I", 1))
    with open(out, "w") as f:
        f.write(f"# amdgpu {os.uname().release}, {len(regs)} registers from {d}\n")
        f.write("\n".join(lines) + "\n")
    if user:
        import pwd
        pw = pwd.getpwnam(user)
        os.chown(out, pw.pw_uid, pw.pw_gid)
    print(f"{len(regs)} registers -> {out}")


if __name__ == "__main__":
    main()
