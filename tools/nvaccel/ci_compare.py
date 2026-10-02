# Core Image outputs: RTX GPU vs M1 GPU (cross machine), RTX GPU vs RTX CPU,
# M1 GPU vs M1 CPU. Run ci_dumpall.sh on both into rtxall/ and m1all/ first;
# the target's x86 CPU renderer differs from the M1 one on some filters, so the
# cross-machine GPU column is the one that counts.
import os, sys
L=[l.strip() for l in open('ci_list.txt') if l.strip()]
def rd(p):
    try: return open(p,'rb').read()
    except: return None
def size(d,f):
    try:
        for l in open(f'{d}/{f}.txt'):
            if l.startswith('size'): w,h=l.split()[1:3]; return int(w),int(h)
    except: pass
    return None
def diff(a,b):
    if a is None or b is None or len(a)!=len(b): return None
    mx=0; s=0
    for x,y in zip(a,b):
        d=abs(x-y); s+=d
        if d>mx: mx=d
    return mx, s/len(a)
def fmt(r): return '-' if r is None else f'{r[0]}/{r[1]:.3f}'
rows=[]
for f in L:
    rg=rd(f'rtxall/{f}.gpu'); rc=rd(f'rtxall/{f}.cpu'); mg=rd(f'm1all/{f}.gpu'); mc=rd(f'm1all/{f}.cpu')
    a=diff(rg,mg); b=diff(rg,rc); c=diff(mg,mc)
    bad_cross = a is None or a[0]>8 or a[1]>0.5
    if bad_cross: rows.append((f,fmt(a),fmt(b),fmt(c), size('rtxall',f), size('m1all',f)))
print(f"{'filter':40s} {'RTXgpu~M1gpu':14s} {'RTXgpu~RTXcpu':14s} {'M1gpu~M1cpu':14s} sizes")
for r in rows: print(f"{r[0]:40s} {r[1]:14s} {r[2]:14s} {r[3]:14s} {r[4]} {r[5]}")
print(len(rows),'of',len(L),'differ from M1 GPU (or missing)')
