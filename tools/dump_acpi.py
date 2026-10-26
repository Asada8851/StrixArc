#!/usr/bin/env python3
import base64
import subprocess
import os
import sys

def dump_acpi_tables(remote_host, out_dir):
    os.makedirs(out_dir, exist_ok=True)

    tables = ["MCFG", "FACP", "VFCT", "IVRS", "HPET", "APIC", "SSDT", "MSDM", "BGRT", "WSMT", "CRAT", "CDIT"]

    ps_script = f"""
$code = @"
using System;
using System.Runtime.InteropServices;
public class FwTable {{
    [DllImport("kernel32.dll")]
    public static extern uint GetSystemFirmwareTable(uint Provider, uint TableID, IntPtr pBuffer, uint BufferSize);
}}
"@
Add-Type -TypeDefinition $code
$sig = 0x41435049 # "ACPI"

foreach ($tbl in @({", ".join([f'"{t}"' for t in tables])})) {{
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($tbl)
    $id = [BitConverter]::ToUInt32($bytes, 0)
    $s = [FwTable]::GetSystemFirmwareTable($sig, $id, [IntPtr]::Zero, 0)
    if ($s -gt 0) {{
        $ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($s)
        [FwTable]::GetSystemFirmwareTable($sig, $id, $ptr, $s)
        $outBytes = New-Object byte[] $s
        [System.Runtime.InteropServices.Marshal]::Copy($ptr, $outBytes, 0, $s)
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
        $b64 = [Convert]::ToBase64String($outBytes)
        Write-Output ("DUMP:" + $tbl + ":" + $s + ":" + $b64)
    }}
}}
"""

    encoded = base64.b64encode(ps_script.encode("utf-16le")).decode("ascii")
    cmd = ["ssh", "-o", "BatchMode=yes", remote_host, f"powershell -EncodedCommand {encoded}"]

    print(f"Connecting to {remote_host} to dump ACPI tables...")
    res = subprocess.run(cmd, capture_output=True)
    if res.returncode != 0:
        print(f"Error executing remote command: {res.stderr.decode('cp866', errors='replace')}")
        return False

    lines = res.stdout.decode("ascii", errors="replace").splitlines()
    for line in lines:
        if line.startswith("DUMP:"):
            parts = line.split(":")
            if len(parts) >= 4:
                tbl_name = parts[1]
                tbl_size = int(parts[2])
                tbl_data = base64.b64decode(parts[3])
                out_path = os.path.join(out_dir, f"{tbl_name}.bin")
                with open(out_path, "wb") as f:
                    f.write(tbl_data)
                print(f"  [SAVED] {tbl_name} ({len(tbl_data)} bytes) -> {out_path}")

    return True

if __name__ == "__main__":
    host = os.environ.get("TARGET_HOST", "user@localhost")
    target_dir = sys.argv[1] if len(sys.argv) > 1 else "hardware/dumps/firebat_u6c/acpi"
    dump_acpi_tables(host, target_dir)
