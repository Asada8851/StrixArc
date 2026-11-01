#!/bin/bash
# install_player.sh: StrixArc's FFmpeg/ffplay (gpu/ffmpeg) as the everyday player that decodes on VCN.
#   /Users/Shared/gpu/ffmpeg/{bin,lib}   ffmpeg, ffprobe, ffplay and their libraries (vkshim as lib/libvulkan.dylib,
#                                        which opens the installed RADV)
#   /usr/local/bin/vplay                 vplay <file>: plays it (VCN when up), one line per file in ~/Library/Logs/vplay.log
#   /Applications/VCN Player.app         opens files from Finder with vplay: Get Info -> Open with -> VCN Player ->
#                                        Change All, or drop files on it
# Nothing else changes; the way back: sudo bash install_player.sh --undo
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu; FF=$G/ffmpeg; VP=/usr/local/bin/vplay; APP="/Applications/VCN Player.app"
die() { echo "STOP: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
if [ "${1:-}" = --undo ]; then
    rm -rf "$FF" "$APP"; rm -f "$VP"; echo "removed $FF, $VP, $APP"; exit 0
fi
[ -x "$HERE/ffmpeg/bin/ffplay" ] || [ -f "$HERE/ffmpeg/bin/ffplay" ] || die "no gpu/ffmpeg/bin/ffplay in this package"
[ -f "$HERE/vplay" ] || die "no gpu/vplay in this package"
rm -rf "$FF.new" && mkdir -p "$FF.new" && cp -R "$HERE/ffmpeg/bin" "$HERE/ffmpeg/lib" "$FF.new/" || die "copy to $FF.new failed"
xattr -dr com.apple.quarantine "$FF.new" 2>/dev/null
for f in "$FF.new"/lib/*.dylib "$FF.new"/bin/*; do codesign --force --sign - "$f" >/dev/null 2>&1; done
chmod 755 "$FF.new"/bin/*; chown -R root:wheel "$FF.new"; chmod -R go-w,a+rX "$FF.new"
rm -rf "$FF" && mv "$FF.new" "$FF" || die "could not put $FF in place"
mkdir -p /usr/local/bin && install -m 755 -o root -g wheel "$HERE/vplay" "$VP" || die "could not install $VP"
# the app: an AppleScript droplet, so Finder hands it the files it opens (a shell script as a bundle executable gets
# no open-document events); each file plays in its own vplay, in the background
rm -rf "$APP"
osacompile -o "$APP" -e 'on open theFiles
    repeat with f in theFiles
        do shell script "/usr/local/bin/vplay " & quoted form of POSIX path of f & " > /dev/null 2>&1 &"
    end repeat
end open
on run
    display dialog "VCN Player plays videos on the Radeon 880M'"'"'s VCN (StrixArc). Open a video with it from Finder (Get Info -> Open with), or drop files on it. Keys in the window: q quit, f full screen, space pause." buttons {"OK"} default button 1
end run' || die "osacompile failed"
PL="$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleName VCN Player" "$PL" 2>/dev/null
/usr/libexec/PlistBuddy -c "Add :CFBundleIdentifier string org.strixarc.vcnplayer" "$PL" 2>/dev/null ||
    /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier org.strixarc.vcnplayer" "$PL"
# the video types it opens, so Finder lists it under Open with
/usr/libexec/PlistBuddy -c "Delete :CFBundleDocumentTypes" "$PL" 2>/dev/null
/usr/libexec/PlistBuddy -c "Add :CFBundleDocumentTypes array" -c "Add :CFBundleDocumentTypes:0 dict" \
    -c "Add :CFBundleDocumentTypes:0:CFBundleTypeName string Video" -c "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Viewer" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes array" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:0 string public.movie" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:1 string public.mpeg-4" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:2 string com.apple.quicktime-movie" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:3 string org.matroska.mkv" "$PL" || die "Info.plist edit failed"
codesign --force --deep --sign - "$APP" >/dev/null 2>&1
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" 2>/dev/null
echo "player installed: $FF, $VP, $APP"
