# ocsmooth.sh (root): boot screen without mode switches or text flashes. Panel is 4K, desktop HiDPI (looks like 1080p):
# (IgnoreTextInGraphics/SanitiseClearScreen/ClearScreenOnModeSwitch are only valid with TextRenderer System*;
# ocvalidate rejects them with BuiltinGraphics.) GOP at Max (4K) + UIScale 2 keeps the logo at the same size and lets the final 4K head match the boot mode.
diskutil mount disk0s1 >/dev/null 2>&1 || exit 1
C=/Volumes/EFI/EFI/OC/config.plist; B=$C.before-smooth-$(date +%Y%m%dT%H%M%S); cp -p $C $B
P=/usr/libexec/PlistBuddy
$P -c "Set :UEFI:Output:Resolution Max" $C
$P -c "Set :UEFI:Output:UIScale 2" $C
plutil -lint $C || { cp -p $B $C; echo "lint failed, restored"; }
$P -c "Print :UEFI:Output" $C | grep -E "Resolution|UIScale|IgnoreText|Sanitise|ClearScreen"
echo "backup $B"
diskutil unmount disk0s1 >/dev/null 2>&1
