#!/bin/bash
# Обновляет утилиты и скрипты в ~/gpu; прошивки в ~/gpu/fw - только если они есть в пакете (gpu/fw, релиз). Без sudo.
# Старые try1..try14 (каждый делал свой bring-up) переезжают в ~/gpu/old.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" != 0 ] || { echo "Запусти без sudo"; exit 1; }
mkdir -p ~/gpu/phxctl ~/gpu/fw ~/gpu/nvmtl
# try1..try14 each did a whole bring-up of their own: with the boot-time LaunchDaemon the GPU is already up and a second
# one rewrites what NMXAccel runs on (mesboot and bringup5.sh now refuse). Kept in ~/gpu/old, out of the way.
mkdir -p ~/gpu/old
for f in ~/gpu/try[0-9]*.sh; do
    [ -e "$f" ] || continue
    case $(basename "$f") in try15.sh) ;; *) mv -f "$f" ~/gpu/old/ ;; esac
done
cp "$HERE"/gpu/*.sh ~/gpu/
cp "$HERE"/gpu/*.txt ~/gpu/ 2>/dev/null || true
cp "$HERE"/gpu/phxctl/* ~/gpu/phxctl/
[ -d "$HERE/gpu/nvmtl" ] && cp "$HERE"/gpu/nvmtl/* ~/gpu/nvmtl/
ls "$HERE"/gpu/fw/*.bin >/dev/null 2>&1 && cp "$HERE"/gpu/fw/*.bin ~/gpu/fw/
mkdir -p ~/gpu/metal && [ -f "$HERE/metal/mtlprobe" ] && cp "$HERE/metal/mtlprobe" ~/gpu/metal/ && chmod +x ~/gpu/metal/mtlprobe
[ -f "$HERE/metal/fpsanim" ] && cp "$HERE/metal/fpsanim" ~/gpu/metal/ && chmod +x ~/gpu/metal/fpsanim
xattr -dr com.apple.quarantine ~/gpu 2>/dev/null || true
chmod +x ~/gpu/*.sh ~/gpu/phxctl/* ~/gpu/nvmtl/vkprobe 2>/dev/null || true
n=$(ls ~/gpu/fw/*.bin 2>/dev/null | wc -l | tr -d ' ')
echo "Прошивок в ~/gpu/fw: $n (нужно 9)"
echo "План загрузки (нужно 17):"; ~/gpu/phxctl/fwplan ~/gpu/fw gc_11_5_0 sdma_6_1_0.bin | grep -vc '^#'
