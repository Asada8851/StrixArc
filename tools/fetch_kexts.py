#!/usr/bin/env python3
import os
import json
import urllib.request
import zipfile
import shutil

KEXT_REPOS = [
    ("acidanthera/Lilu", "Lilu"),
    ("acidanthera/VirtualSMC", "VirtualSMC"),
    ("acidanthera/AppleALC", "AppleALC"),
    ("acidanthera/VoodooPS2", "VoodooPS2"),
    ("acidanthera/NVMeFix", "NVMeFix"),
    ("acidanthera/RestrictEvents", "RestrictEvents"),
    ("VoodooI2C/VoodooI2C", "VoodooI2C"),
    ("ChefKissInc/NootedRed", "NootedRed"),
    ("trulyspinach/SMCAMDProcessor", "SMCAMDProcessor")
]

OUT_DIR = "hardware/efi_firebat_u6c/EFI/OC/Kexts"
TMP_DIR = "/tmp/kext_dl"
os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(TMP_DIR, exist_ok=True)

def download_kext(repo, name):
    url = f"https://api.github.com/repos/{repo}/releases/latest"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode())
    except Exception as e:
        print(f"Failed to query {repo}: {e}")
        return

    dl_url = None
    for asset in data.get("assets", []):
        aname = asset.get("name", "")
        if "RELEASE" in aname and aname.endswith(".zip"):
            dl_url = asset.get("browser_download_url")
            break
        elif not dl_url and aname.endswith(".zip") and "DEBUG" not in aname:
            dl_url = asset.get("browser_download_url")

    if not dl_url:
        print(f"No zip found for {repo}")
        return

    zip_path = os.path.join(TMP_DIR, f"{name}.zip")
    print(f"Downloading {name} from {dl_url}...")
    try:
        urllib.request.urlretrieve(dl_url, zip_path)
    except Exception as e:
        print(f"Download error {name}: {e}")
        return

    extract_path = os.path.join(TMP_DIR, name)
    shutil.rmtree(extract_path, ignore_errors=True)
    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(extract_path)

    # find all .kext folders inside extract_path
    for root, dirs, files in os.walk(extract_path):
        for d in dirs:
            if d.endswith(".kext"):
                src_kext = os.path.join(root, d)
                dst_kext = os.path.join(OUT_DIR, d)
                if os.path.exists(dst_kext):
                    shutil.rmtree(dst_kext)
                print(f"  -> Installing {d}")
                shutil.copytree(src_kext, dst_kext)

if __name__ == "__main__":
    for repo, name in KEXT_REPOS:
        download_kext(repo, name)
    print("Done fetching kexts.")
