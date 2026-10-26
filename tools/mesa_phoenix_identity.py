#!/usr/bin/env python3
"""
tools/mesa_phoenix_identity.py — report the Firebat U6C's real GPU identity when the canned
"phoenix" table is selected: "AMD Radeon 780M Graphics", PCI 1002:1900 rev 0xBA, subsystem 8001:2014
(values from hardware/dumps/firebat_u6c/pci_resources.json). gfx1201 keeps Cobalt64's RX 9070 XT identity.

Run from the Mesa root after tools/mesa_phoenix_canned.py.
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
    radv = ROOT / "src/amd/vulkan/radv_physical_device.c"

    d = drm.read_text()
    d = replace_once(
        d,
        "acd_get_marketing_name(acd_device_handle dev)\n{\n   return ACD_MARKETING_NAME;\n}",
        "acd_get_marketing_name(acd_device_handle dev)\n{\n"
        "   if (!strcmp(dev->canned->name, \"phoenix\"))\n"
        "      return \"AMD Radeon 780M Graphics\";\n"
        "   return ACD_MARKETING_NAME;\n}",
        drm,
    )
    d = replace_once(
        d,
        "   case AMDGPU_INFO_DEV_INFO:\n      *(struct drm_amdgpu_info_device *)out = d->dev;\n      return 0;",
        "   case AMDGPU_INFO_DEV_INFO: {\n"
        "      struct drm_amdgpu_info_device *di = out;\n"
        "      *di = d->dev;\n"
        "      if (!strcmp(d->name, \"phoenix\"))\n"
        "         di->device_id = 0x1900; /* Hawk Point 780M; same gfx1103 silicon as the canned 0x15bf */\n"
        "      return 0;\n"
        "   }",
        drm,
    )

    r = radv.read_text()
    r = replace_once(
        r,
        "   /* Fake PCI identity of the RX 9070 XT (vendor 0x1002, device 0x7550). */\n"
        "   static char *nodes[DRM_NODE_MAX];\n"
        "   static drmPciBusInfo bus_info;\n"
        "   static drmPciDeviceInfo device_info = {\n"
        "      .vendor_id = 0x1002,\n"
        "      .device_id = 0x7550,\n"
        "      .subvendor_id = 0x1002,\n"
        "      .subdevice_id = 0x0000,\n"
        "      .revision_id = 0xC0,\n"
        "   };",
        "   /* PCI identity of the canned device: RX 9070 XT (1002:7550) for gfx1201, otherwise the\n"
        "    * Firebat U6C's Radeon 780M (1002:1900 rev 0xBA, subsystem 8001:2014). */\n"
        "   static char *nodes[DRM_NODE_MAX];\n"
        "   static drmPciBusInfo bus_info;\n"
        "   static drmPciDeviceInfo device_info = {\n"
        "      .vendor_id = 0x1002,\n"
        "      .device_id = 0x1900,\n"
        "      .subvendor_id = 0x8001,\n"
        "      .subdevice_id = 0x2014,\n"
        "      .revision_id = 0xBA,\n"
        "   };\n"
        "   const char *canned = getenv(\"RADV_DARWIN_CANNED\");\n"
        "   if (canned && !strcmp(canned, \"gfx1201\")) {\n"
        "      device_info.device_id = 0x7550;\n"
        "      device_info.subvendor_id = 0x1002;\n"
        "      device_info.subdevice_id = 0x0000;\n"
        "      device_info.revision_id = 0xC0;\n"
        "   }",
        radv,
    )

    drm.write_text(d)
    radv.write_text(r)
    print("identity: phoenix -> AMD Radeon 780M Graphics 1002:1900 rev 0xBA")


if __name__ == "__main__":
    main()
