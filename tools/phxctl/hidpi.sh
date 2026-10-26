#!/bin/bash
# HiDPI ("Retina") modes for the 880M's 3072x1920 panel once the firmware frame is native 3K: a display override with
# scale-resolutions (the one-key-hidpi way: each entry is twice the size it looks like, flagged HiDPI), so System
# Settings -> Displays offers "looks like 1536x960" etc. instead of tiny 1:1 text. Takes effect at the next login/boot.
#   sudo bash hidpi.sh          install     sudo bash hidpi.sh --undo     remove
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
VID=$(ioreg -lw0 | sed -n 's/.*"DisplayVendorID" = \([0-9]*\).*/\1/p' | head -1)
PID=$(ioreg -lw0 | sed -n 's/.*"DisplayProductID" = \([0-9]*\).*/\1/p' | head -1)
[ -n "$VID" ] && [ -n "$PID" ] || { echo "STOP: macOS reports no DisplayVendorID/DisplayProductID for the panel"; exit 1; }
DIR=/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-$(printf '%x' "$VID")
F=$DIR/DisplayProductID-$(printf '%x' "$PID")
if [ "${1:-}" = --undo ]; then rm -f "$F"; echo "removed $F (log out or reboot)"; exit 0; fi
entry() { printf '%08x%08x%08x%s' $(($1 * 2)) $(($2 * 2)) 1 "$3" | xxd -r -p | base64; }
{
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
    echo '<plist version="1.0"><dict>'
    echo "  <key>DisplayProductID</key><integer>$PID</integer>"
    echo "  <key>DisplayVendorID</key><integer>$VID</integer>"
    echo '  <key>DisplayProductName</key><string>nullarc 880M panel</string>'
    echo '  <key>scale-resolutions</key><array>'
    for r in 1536x960 1440x900 1680x1050 1280x800 1920x1200 1024x640; do
        w=${r%x*}; h=${r#*x}
        echo "    <data>$(entry $w $h '')</data>"
        echo "    <data>$(entry $w $h 00200000)</data>"
    done
    echo '  </array>'
    echo '</dict></plist>'
} > /tmp/nullarc-hidpi.plist
plutil -lint /tmp/nullarc-hidpi.plist >/dev/null || { echo "STOP: the generated plist is not valid"; exit 1; }
mkdir -p "$DIR"; install -m 644 /tmp/nullarc-hidpi.plist "$F"
echo "installed $F (vendor $VID, product $PID). Log out or reboot, then System Settings -> Displays: pick 'looks like 1536x960'."
echo "undo: sudo bash $0 --undo"
