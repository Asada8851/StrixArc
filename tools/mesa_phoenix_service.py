#!/usr/bin/env python3
"""
tools/mesa_phoenix_service.py — make the RADV Darwin backend open the Phoenix bring-up kext's
service (class "PhxBringup", src/phxbringup) by default. The Navi48 class names stay accepted
by acd_kext_set_service_class(). Run from the Mesa root after the other mesa_phoenix_* scripts.
"""

import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")


def replace_once(text, old, new, path):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected 1 match, found {count}:\n{old}")
    return text.replace(old, new)


def main():
    hdr = ROOT / "src/amd/common/darwin/ac_darwin_kext.h"
    src = ROOT / "src/amd/common/darwin/ac_darwin_kext.c"

    h = hdr.read_text()
    h = replace_once(
        h,
        '#define ACD_SERVICE_ACCELERATOR "Navi48Accelerator"',
        '#define ACD_SERVICE_ACCELERATOR "Navi48Accelerator"\n'
        '#define ACD_SERVICE_PHOENIX     "PhxBringup" /* Radeon 780M bring-up kext (src/phxbringup) */',
        hdr,
    )

    s = src.read_text()
    s = replace_once(
        s,
        "static const char *volatile acd_service_class = ACD_SERVICE_BRINGUP;",
        "static const char *volatile acd_service_class = ACD_SERVICE_PHOENIX;",
        src,
    )
    s = replace_once(
        s,
        "   else if (!strcmp(cls, ACD_SERVICE_ACCELERATOR))\n      acd_service_class = ACD_SERVICE_ACCELERATOR;",
        "   else if (!strcmp(cls, ACD_SERVICE_ACCELERATOR))\n      acd_service_class = ACD_SERVICE_ACCELERATOR;\n"
        "   else if (!strcmp(cls, ACD_SERVICE_PHOENIX))\n      acd_service_class = ACD_SERVICE_PHOENIX;",
        src,
    )

    hdr.write_text(h)
    src.write_text(s)
    print("default service class: PhxBringup")


if __name__ == "__main__":
    main()
