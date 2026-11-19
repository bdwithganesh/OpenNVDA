# disp_state.sh (root): move disp_all state and scratch files from /tmp to a root-owned dir (symlink-safe).
S=/Library/NativeDriverLab/display/state
mkdir -p $S && chown root:wheel $S && chmod 755 $S
T=$(date +%s)
for f in /Library/NativeDriverLab/display/hotplug-watch.sh /Users/dhruveen/nvbuild/d615/disp_all.py /Users/dhruveen/nvbuild/d615/disp_fix1.py \
         /Users/dhruveen/nvbuild/d615/disp_replug.py /Users/dhruveen/nvbuild/d615/disp_sysfb.py /Users/dhruveen/nvbuild/d615/olut_kick.py \
         /Users/dhruveen/nvbuild/d615/olut_live.py /Users/dhruveen/nvbuild/perf/olut_kick.py /Users/dhruveen/nvbuild/perf/hotplug-watch.dbl.sh; do
  [ -f "$f" ] && [ ! -L "$f" ] || continue
  cp -p "$f" "$f.before-state-$T"
  sed -i '' -e "s#/tmp/disp615#$S/disp615#g" -e "s#/tmp/zero.bin#$S/zero.bin#g" -e "s#/tmp/lut.bin#$S/lut.bin#g" \
            -e "s#/tmp/cursor.bin#$S/cursor.bin#g" -e "s#/tmp/full.bin#$S/full.bin#g" "$f"
  echo "$f: $(grep -c '/tmp/' "$f") /tmp left"
done
P=/Users/dhruveen/nvbuild/d615/disp_all.py
/usr/bin/python3 - "$P" <<'PYEOF'
import sys; p=sys.argv[1]; s=open(p).read()
old="    inst, corepb = partial['inst'], partial['core_pb']\n"
assert s.count(old)==1
s=s.replace(old,old+"    if not (isinstance(inst, int) and isinstance(corepb, int) and inst > 0 and corepb > 0):\n        sys.exit('resume: bad partial state')\n")
open(p,'w').write(s)
PYEOF
/usr/bin/python3 -m py_compile $P && echo compiled
for f in /Library/NativeDriverLab/display/hotplug-watch.sh /Users/dhruveen/nvbuild/perf/hotplug-watch.dbl.sh; do sh -n $f && echo "$f syntax-ok"; done
# carry the current state over so hotplug and the tools keep working this boot
for x in disp615.json disp615-partial.json; do [ -f /tmp/$x ] && [ ! -L /tmp/$x ] && cp /tmp/$x $S/$x && chmod 644 $S/$x; done; ls -l $S
launchctl kickstart -k system/org.local.nvdisphotplug && echo hotplug-restarted
