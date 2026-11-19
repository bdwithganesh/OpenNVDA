# QuickTime CPU while playing the 1080p clips: NVDEC (default) vs software (nvmtl-nvvt-off)
# Run with sudo (toggles /Library/Preferences/nvmtl-nvvt-off). Clips from mkclip.m: clip_h264.mp4, clip_hevc.mp4, clip_hevc10.mp4 (main10).
run() {
  for c in h264 hevc hevc10; do
    sudo -u "${CLIP_USER:-$SUDO_USER}" osascript -e 'tell application "QuickTime Player" to quit' >/dev/null 2>&1; sleep 2
    sudo -u "${CLIP_USER:-$SUDO_USER}" open -a "QuickTime Player" ${CLIPS:-/Users/$SUDO_USER/Movies/nvtest}/clip_$c.mp4; sleep 4
    sudo -u "${CLIP_USER:-$SUDO_USER}" osascript -e 'tell application "QuickTime Player" to play document 1' >/dev/null 2>&1; sleep 3
    P=$(pgrep -x "QuickTime Player")
    T=$(sudo -u "${CLIP_USER:-$SUDO_USER}" osascript -e 'tell application "QuickTime Player" to get current time of document 1' 2>&1); S=$(for i in 1 2 3 4 5 6; do sleep 1; ps -o %cpu= -p $P; done | awk '{s+=$1} END {printf "%.1f", s/NR}')
    A=$(for i in 1 2 3 4 5 6; do sleep 1; ps -A -o %cpu= | awk '{s+=$1} END {print s}'; done | awk '{s+=$1} END {printf "%.1f", s/NR}')
    X=$(ps -A -o %cpu=,comm= | grep -E "VTDecoderXPC|WindowServer|mediaserverd" | awk '{printf "%s=%s ", $2, $1}' | sed 's#/[^ =]*/##g')
    echo "$1 $c: QuickTime CPU avg ${S} %, all processes ${A} % (t=$T) $X"
  done
  sudo -u "${CLIP_USER:-$SUDO_USER}" osascript -e 'tell application "QuickTime Player" to quit' >/dev/null 2>&1
}
rm -f /Library/Preferences/nvmtl-nvvt-off; run nvdec
touch /Library/Preferences/nvmtl-nvvt-off; run software
rm -f /Library/Preferences/nvmtl-nvvt-off
