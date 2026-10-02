# regen.py PATCH FILE... : refresh those files' sections of PATCH from ~/nvk/mesa-pr1
import os, re, subprocess, sys, tempfile
patch, oldp, files = sys.argv[1], sys.argv[2], sys.argv[3:]
old = {x.split()[2][2:]: x for x in re.split(r"(?m)^(?=diff --git )", open(oldp).read()) if x}
home = os.path.expanduser("~/nvk")
t = open(patch).read()
secs = [s for s in re.split(r'(?m)^(?=diff --git )', t) if s]
def path_of(s): return s.split()[2][2:]
by = {path_of(s): i for i, s in enumerate(secs)}
for f in files:
    new = os.path.join(home, "mesa-pr1", f)
    orig = os.path.join(home, "mesa-orig", f)
    tmp = tempfile.mkdtemp()
    base = os.path.join(tmp, "base")
    if (f in by and "new file mode" in secs[by[f]].split("\n@@")[0]) or not os.path.exists(orig):
        base = "/dev/null"   # a file the patch adds
    else:
        open(base, "w").write(open(orig).read())
        if f in old:   # mesa-orig carries the old patch for this file: take it off
            sp = os.path.join(tmp, "s.patch"); open(sp, "w").write(old[f])
            r = subprocess.run(["patch", "-R", base, sp], capture_output=True, text=True)
            if r.returncode: sys.exit(f"reverse failed for {f}: {r.stdout}{r.stderr}")
    d = subprocess.run(["diff", "-u", base, new], capture_output=True, text=True).stdout.split("\n", 2)[2]
    hdr = f"diff --git a/{f} b/{f}\n" + ("new file mode 100644\n--- /dev/null\n" if base == "/dev/null" else f"--- a/{f}\n") + f"+++ b/{f}\n"
    sec = hdr + d
    if f in by: secs[by[f]] = sec
    else:
        i = 0
        while i < len(secs) and path_of(secs[i]) < f: i += 1
        secs.insert(i, sec); by = {path_of(s): j for j, s in enumerate(secs)}
    print("refreshed", f)
open(patch, "w").write("".join(secs))
