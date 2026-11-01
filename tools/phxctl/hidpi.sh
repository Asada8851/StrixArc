#!/bin/bash
# HiDPI ("Retina") modes for the 880M's 3072x1920 panel: a display override with scale-resolutions (the one-key-hidpi
# way: each entry is twice the size it looks like, flagged HiDPI), so System Settings -> Displays offers "looks like
# 1536x960" etc. instead of tiny 1:1 text. Takes effect at the next login/boot.
#   sudo bash hidpi.sh            install (the override for the IDs CoreGraphics reports, and ioreg's if they differ)
#   bash hidpi.sh --check         after the reboot: the IDs, the overrides, every mode macOS offers (HiDPI marked)
#   sudo bash hidpi.sh --undo     remove
# v1 (2026-10-27) took the first DisplayVendorID ioreg showed and no mode appeared: v2 takes the main display's
# numbers from CoreGraphics (what the override is matched against), lists them all, and logs to the USB stick.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LM=~/gpu/metal/listmodes; [ -x "$HERE/../metal/listmodes" ] && LM="$HERE/../metal/listmodes"
[ -x "$LM" ] || LM=""
LOGS=~/gpu/nullarc_logs.sh; [ -f "$HERE/nullarc_logs.sh" ] && LOGS="$HERE/nullarc_logs.sh"
OV=/Library/Displays/Contents/Resources/Overrides
LOG=/tmp/nullarc_hidpi.txt
say() { echo "$1"; echo "$1" >> "$LOG"; }
: > "$LOG"; chmod 666 "$LOG" 2>/dev/null
trap '[ -f "$LOGS" ] && bash "$LOGS" hidpi "$LOG" | tail -1' EXIT
say "== $(date '+%F %T') hidpi.sh ${1:-install}"

# the IDs: CoreGraphics' for the main display, and every IODisplay's in ioreg
CG=""; [ -n "$LM" ] && CG=$("$LM" --ids 2>/dev/null)
# (paired per object: ioreg does not keep the keys in one order)
ior_ids() { awk '/\+-o /{ if (v != "" && p != "") print v, p; v = p = "" }
    /"DisplayVendorID" = [0-9]/ { match($0, /"DisplayVendorID" = [0-9]+/); v = substr($0, RSTART + 20, RLENGTH - 20) }
    /"DisplayProductID" = [0-9]/ { match($0, /"DisplayProductID" = [0-9]+/); p = substr($0, RSTART + 21, RLENGTH - 21) }
    END { if (v != "" && p != "") print v, p }' | sort -u; }
IOR=$(ioreg -lw0 -r -c IODisplayConnect -d 3 | ior_ids)
say "CoreGraphics main display: vendor/model ${CG:-? (no listmodes)}"
say "ioreg IODisplay vendor/product: $(echo "$IOR" | tr ' \n' '/ ')"

show() {
    say "== overrides"
    find "$OV" -type f 2>/dev/null | while IFS= read -r f; do say "   $f"; plutil -p "$f" 2>/dev/null | grep -E 'DisplayVendorID|DisplayProductID|ProductName' | sed 's/^/     /' | tee -a "$LOG"; done
    say "   DisplayResolutionEnabled: $(defaults read /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled 2>/dev/null || echo unset)"
    if [ -n "$LM" ]; then say "== modes"; "$LM" | tee -a "$LOG"
        if "$LM" | grep -q ' HiDPI'; then say "== HiDPI modes are there: System Settings -> Displays (Show all resolutions) -> 1536x960"
        else say "== NO HiDPI mode: macOS did not take the override for this framebuffer"; fi
    fi
}
if [ "${1:-}" = --check ]; then show; exit 0; fi
[ "$(id -u)" = 0 ] || { echo "run with sudo (or: bash $0 --check)"; exit 1; }
if [ "${1:-}" = --undo ]; then
    for p in $( { echo "$CG"; echo "$IOR"; } | sed '/^$/d' | tr ' ' ':'); do
        f=$OV/DisplayVendorID-$(printf '%x' "${p%%:*}")/DisplayProductID-$(printf '%x' "${p##*:}")
        grep -q 'nullarc 880M panel' "$f" 2>/dev/null && rm -f "$f" && say "removed $f"
    done
    say "log out or reboot"; exit 0
fi

entry() { printf '%08x%08x%08x%s' $(($1 * 2)) $(($2 * 2)) 1 "$3" | xxd -r -p | base64; }
write_override() {   # vendor product
    local d=$OV/DisplayVendorID-$(printf '%x' "$1") f
    f=$d/DisplayProductID-$(printf '%x' "$2")
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
        echo '<plist version="1.0"><dict>'
        echo "  <key>DisplayProductID</key><integer>$2</integer>"
        echo "  <key>DisplayVendorID</key><integer>$1</integer>"
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
    plutil -lint /tmp/nullarc-hidpi.plist >/dev/null || { say "STOP: the generated plist is not valid"; exit 1; }
    mkdir -p "$d"; install -m 644 -o root -g wheel /tmp/nullarc-hidpi.plist "$f"
    say "installed $f (vendor $1, product $2)"
}
done_ids=""
for p in $( { echo "$CG"; echo "$IOR"; } | sed '/^$/d' | tr ' ' ':'); do
    case " $done_ids " in *" $p "*) continue ;; esac
    write_override "${p%%:*}" "${p##*:}"; done_ids="$done_ids $p"
done
[ -n "$done_ids" ] || { say "STOP: neither CoreGraphics nor ioreg gives the panel's vendor/product"; exit 1; }
# older macOS lists HiDPI modes only with this (one-key-hidpi sets it too); harmless on 15
defaults write /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled -bool true
show
say "== reboot, then: bash ~/gpu/hidpi.sh --check (and System Settings -> Displays -> Show all resolutions)"
say "   undo: sudo bash ~/gpu/hidpi.sh --undo"
