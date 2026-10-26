#!/usr/bin/env python3
"""
tools/mesa_phoenix_canned.py — retarget the Cobalt64 RADV Darwin backend from the canned
gfx1201 table to a selectable one (default "phoenix", gfx1103 / GC 11.0.1).

Run from the root of a Mesa tree that has Cobalt64's mesa-patches 0001..0009 applied.
Each replacement must match exactly once, otherwise the script stops without writing.
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
    drm = ROOT / "src/amd/common/darwin/ac_darwin_drm.c"
    mock = ROOT / "src/amd/common/darwin/ac_darwin_mock.c"
    hdr = ROOT / "src/amd/common/darwin/ac_darwin_drm.h"

    d = drm.read_text()
    d = replace_once(
        d,
        "static const struct amdgpu_device *\nfind_canned(const char *name)\n{",
        "static const struct amdgpu_device *\nfind_canned(const char *name);\n\n"
        "/* Canned device selection: RADV_DARWIN_CANNED names an amdgpu_devices[] entry, default \"phoenix\"\n"
        " * (Radeon 780M / gfx1103 / GC 11.0.1). The values that used to be gfx1201 constants come from it. */\n"
        "const struct amdgpu_device *\nacd_canned_device(void)\n{\n"
        "   const char *name = getenv(\"RADV_DARWIN_CANNED\");\n"
        "   return find_canned(name && *name ? name : \"phoenix\");\n}\n\n"
        "uint32_t\nacd_canned_gb_addr_config(const struct amdgpu_device *d)\n{\n"
        "   for (uint32_t i = 0; i + 2 < ARRAY_SIZE(d->mmr_regs) && d->mmr_regs[i]; i += 3) {\n"
        "      if (d->mmr_regs[i] == ACD_GB_ADDR_CONFIG_REG)\n"
        "         return d->mmr_regs[i + 2];\n"
        "   }\n"
        "   return 0;\n}\n\n"
        "uint32_t\nacd_canned_gc_version(const struct amdgpu_device *d)\n{\n"
        "   switch (d->dev.family) {\n"
        "   case AMDGPU_FAMILY_GC_11_0_1: return (11u << 16) | (0u << 8) | 1u;\n"
        "   case AMDGPU_FAMILY_GC_12_0_0: return (12u << 16) | (0u << 8) | 1u;\n"
        "   default: return 0;\n"
        "   }\n}\n\n"
        "static const struct amdgpu_device *\nfind_canned(const char *name)\n{",
        drm,
    )
    d = replace_once(
        d,
        "   if (gb != ACD_GB_ADDR_CONFIG_GFX1201 && !env_is_1(\"RADV_DARWIN_ALLOW_ADDRCFG\")) {\n"
        "      fprintf(stderr, \"radv-darwin: live GB_ADDR_CONFIG 0x%08x != canned gfx1201 0x%08x (set \"\n"
        "                      \"RADV_DARWIN_ALLOW_ADDRCFG=1 to override)\\n\",\n"
        "              gb, ACD_GB_ADDR_CONFIG_GFX1201);",
        "   if (gb != acd_canned_gb_addr_config(canned) && !env_is_1(\"RADV_DARWIN_ALLOW_ADDRCFG\")) {\n"
        "      fprintf(stderr, \"radv-darwin: live GB_ADDR_CONFIG 0x%08x != canned %s 0x%08x (set \"\n"
        "                      \"RADV_DARWIN_ALLOW_ADDRCFG=1 to override)\\n\",\n"
        "              gb, canned->name, acd_canned_gb_addr_config(canned));",
        drm,
    )
    d = replace_once(d, '   dev->canned = find_canned("gfx1201");', "   dev->canned = acd_canned_device();", drm)

    m = mock.read_text()
    m = replace_once(m, '#include "ac_darwin_kext.h"\n', '#include "ac_darwin_kext.h"\n#include "ac_darwin_drm.h"\n', mock)
    m = replace_once(
        m,
        "      inf.gb_addr_config = ACD_GB_ADDR_CONFIG_GFX1201;\n"
        "      inf.gc_version = (12u << 16) | (0u << 8) | 1u;",
        "      inf.gb_addr_config = acd_canned_gb_addr_config(acd_canned_device());\n"
        "      inf.gc_version = acd_canned_gc_version(acd_canned_device());",
        mock,
    )
    m = replace_once(
        m,
        "v[i] = (sin[0] + i == ACD_GB_ADDR_CONFIG_REG) ? ACD_GB_ADDR_CONFIG_GFX1201 : 0;",
        "v[i] = (sin[0] + i == ACD_GB_ADDR_CONFIG_REG)\n"
        "                    ? acd_canned_gb_addr_config(acd_canned_device()) : 0;",
        mock,
    )

    h = hdr.read_text()
    marker = "\n#ifdef __cplusplus\n}"
    proto = (
        "\nstruct amdgpu_device;\n"
        "const struct amdgpu_device *acd_canned_device(void);\n"
        "uint32_t acd_canned_gb_addr_config(const struct amdgpu_device *d);\n"
        "uint32_t acd_canned_gc_version(const struct amdgpu_device *d);\n"
    )
    if h.count(marker) == 1:
        h = h.replace(marker, proto + marker)
    else:
        h = h.rstrip() + "\n" + proto

    drm.write_text(d)
    mock.write_text(m)
    hdr.write_text(h)
    print("retargeted canned device: default phoenix (RADV_DARWIN_CANNED overrides)")


if __name__ == "__main__":
    main()
