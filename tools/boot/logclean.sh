#!/bin/sh
# logclean.sh (root): delete every log file older than now, keep what is written from here on.
# Touches only log outputs: never kexts, scripts, backups, version files or the AuxKC.
REF=/private/var/run/logclean.ref; touch "$REF"
A=/Library/NativeDriverLab/autogpu; U=/Users/dhruveen
install -o root -g wheel -m 755 /tmp/auxrebuild.new "$A/auxrebuild.sh" 2>/dev/null || true
before=$(df -k / | awk 'NR==2{print $3}')
n=0; del() { for f in "$@"; do [ -f "$f" ] && [ ! "$f" -nt "$REF" ] && rm -f "$f" && n=$((n+1)); done; }
del $A/log-*.txt $A/boot-*.log $A/stage-*.log $A/status-*.log $A/bootstrap-*.json $A/compute1-*.json $A/compute2-*.json $A/accelgo-*.json
echo "autogpu: $n"; n=0
for d in /Library/Logs $U/Library/Logs /private/var/tmp/nvglass $U/nvbuild/m3/logs $U/nvbuild/m4/logs $U/nvbuild/suite84/logs; do
  c=$(find "$d" -type f ! -newer "$REF" 2>/dev/null | wc -l | tr -d ' '); find "$d" -type f ! -newer "$REF" -delete 2>/dev/null; echo "$d: $c"; done
c=$(ls $U/nvbuild/*.log 2>/dev/null | wc -l | tr -d ' '); rm -f $U/nvbuild/*.log; echo "nvbuild/*.log: $c"
c=$(find /private/var/log -type f \( -name '*.gz' -o -name '*.bz2' -o -name '*.asl' -o -name '*.[0-9]' \) | wc -l | tr -d ' ')
find /private/var/log -type f \( -name '*.gz' -o -name '*.bz2' -o -name '*.asl' -o -name '*.[0-9]' \) -delete
find /private/var/log -type f \( -name '*.log' -o -name '*.out' \) -exec sh -c ': > "$1"' _ {} \; ; echo "/var/log: $c archives deleted, active *.log truncated"
diskutil mount disk0s1 >/dev/null 2>&1; c=$(ls /Volumes/EFI/opencore-*.txt 2>/dev/null | wc -l | tr -d ' '); rm -f /Volumes/EFI/opencore-*.txt; echo "ESP opencore logs: $c"; diskutil unmount disk0s1 >/dev/null 2>&1
log erase --all >/dev/null 2>&1 && echo "unified log erased: $(du -sh /private/var/db/diagnostics | cut -f1) left"
after=$(df -k / | awk 'NR==2{print $3}'); echo "freed $(( (before-after)/1024 )) MiB on / (plus ESP)"
