#!/bin/sh
# ocquiet.sh (root): quiet, fast boot. OpenCore: no onscreen/file log, no boot.efi debug log, no picker
# (Option still opens it). boot-args: drop -v, serial/debug and the MacHyperVSupport debug flags. Backup on the ESP.
set -eu
diskutil mount disk0s1 >/dev/null 2>&1 || true
C=/Volumes/EFI/EFI/OC/config.plist
B=/Volumes/EFI/EFI/OC/config.plist.before-quiet-$(date +%Y%m%dT%H%M%S)
cp -p "$C" "$B"
PB=/usr/libexec/PlistBuddy
$PB -c "Set :Misc:Debug:Target 0" "$C"
$PB -c "Set :Misc:Debug:AppleDebug false" "$C"
$PB -c "Set :Misc:Boot:ShowPicker false" "$C"
$PB -c "Set :Misc:Boot:PollAppleHotKeys true" "$C"
OLD=$($PB -c "Print :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args" "$C")
NEW=$(printf '%s\n' "$OLD" | tr ' ' '\n' | grep -vxE -- '-v|debug=0x100|serial=3|-hvpcibdbg|-hvpcibmsgdbg|-hvpcirdbg' | tr '\n' ' ' | sed 's/ $//')
case "$NEW" in *-hvgfxoff*) ;; *) echo "lost -hvgfxoff, abort"; cp -p "$B" "$C"; exit 1;; esac
$PB -c "Set :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args $NEW" "$C"
plutil -lint "$C"
echo "backup: $B"
echo "boot-args: $($PB -c "Print :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args" "$C")"
for k in Misc:Debug:Target Misc:Debug:AppleDebug Misc:Boot:ShowPicker Misc:Boot:PollAppleHotKeys; do echo "$k = $($PB -c "Print :$k" "$C")"; done
nvram boot-args="$NEW"   # same value now, in case OpenCore's delete/add is skipped
diskutil unmount disk0s1 >/dev/null 2>&1 || true
