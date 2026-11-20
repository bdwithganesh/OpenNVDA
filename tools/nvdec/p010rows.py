# p010rows.py SW HW W H: which HW luma rows are zero / match which SW rows
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read(); W = int(sys.argv[3]) * 2; H = int(sys.argv[4])
ra = [a[y * W:(y + 1) * W] for y in range(H)]; rb = [b[y * W:(y + 1) * W] for y in range(H)]
idx = {}
for y, r in enumerate(ra): idx.setdefault(r, y)
zero = bytes(W)
out = []
for y in range(0, H, 4):
    r = rb[y]
    m = idx.get(r)
    half = 'L' if r[:W // 2] == ra[y][:W // 2] else '-'
    out.append('%d:%s%s' % (y, 'Z' if r == zero else ('=' if r == ra[y] else ('sw%d' % m if m is not None else '?')), half))
print(' '.join(out[:80]))
nz = [y for y in range(H) if rb[y] != zero]
print('nonzero hw rows', len(nz), 'last', nz[-1] if nz else None, 'exact rows', sum(1 for y in range(H) if ra[y] == rb[y]))
# chroma plane
ca = a[W * H:]; cb = b[W * H:]
print('chroma exact rows', sum(1 for y in range(H // 2) if ca[y * W:(y + 1) * W] == cb[y * W:(y + 1) * W]), 'of', H // 2,
      'zero rows', sum(1 for y in range(H // 2) if cb[y * W:(y + 1) * W] == zero))
