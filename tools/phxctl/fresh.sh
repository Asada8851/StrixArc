#!/usr/bin/env bash
# usage: fresh.sh "<remote cmd>"  -> reboot Mac, wait for kext, run command
# Uses standard SSH public-key authentication without embedded passwords.
# If passwordless reboot is required on the target machine, configure:
#   %admin ALL=(ALL) NOPASSWD: /sbin/shutdown
# in /etc/sudoers.d/phxctl on the target macOS machine.

set -euo pipefail

SSH_USER="${MAC_SSH_USER:-admin}"
IP="${MAC_IP:?set MAC_IP to the test Mac address, e.g. MAC_IP=10.0.0.5}"
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10)

echo "[phxctl] Requesting remote reboot on ${SSH_USER}@${IP}..."
ssh "${SSH_OPTS[@]}" "${SSH_USER}@${IP}" "sudo shutdown -r now" >/dev/null 2>&1 || true

echo "[phxctl] Waiting for system to reboot (75s)..."
sleep 75

echo "[phxctl] Polling for PhxBringup kext registration..."
for i in $(seq 1 45); do
    if ssh "${SSH_OPTS[@]}" "${SSH_USER}@${IP}" 'ioreg -l | grep -c "phx-build"' 2>/dev/null | grep -q '^[1-9]'; then
        echo "[phxctl] PhxBringup detected active after probe #${i}."
        break
    fi
    sleep 8
done

if [ $# -gt 0 ]; then
    echo "[phxctl] Executing payload: $1"
    ssh "${SSH_OPTS[@]}" "${SSH_USER}@${IP}" "$1"
fi
