#!/bin/bash

SUDO_CMD="sudo"
if [ -n "$SUDO_PASS" ]; then
    SUDO_CMD="echo '$SUDO_PASS' | sudo -S -p ''"
fi

# On the Mac: build PhxBringup.kext from ~/gpu/phxbringup and put it into the OpenCore EFI (active from the next boot).
set -u
cd ~/gpu/phxbringup && sh build.sh ~/gpu/MacKernelSDK 2>&1 | grep -E " error|built" || exit 1
codesign --force --sign - build/PhxBringup.kext >/dev/null 2>&1
eval "$SUDO_CMD" diskutil mount disk0s1 >/dev/null 2>&1
eval "$SUDO_CMD" rm -rf /Volumes/EFI/EFI/OC/Kexts/PhxBringup.kext
eval "$SUDO_CMD" cp -R build/PhxBringup.kext /Volumes/EFI/EFI/OC/Kexts/ && echo "deployed build $(grep -o 'PHX_KEXT_BUILD [0-9]*' PhxBringup.cpp | cut -d' ' -f2)"
