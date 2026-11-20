# p010cmp.py A B: compare two raw 16-bit (P010-style) frame dumps
import sys, struct
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
print('bytes', len(a), len(b))
n = min(len(a), len(b)) // 2
x = struct.unpack('<%dH' % n, a[:n * 2]); y = struct.unpack('<%dH' % n, b[:n * 2])
eq = sum(1 for i in range(n) if x[i] == y[i])
eq10 = sum(1 for i in range(n) if x[i] >> 6 == y[i] >> 6)
low_a = sum(1 for v in x if v & 63); low_b = sum(1 for v in y if v & 63)
d = [abs((x[i] >> 6) - (y[i] >> 6)) for i in range(n)]
print('samples', n, 'equal16', eq, 'equal10(msb)', eq10, 'lowbits set a', low_a, 'b', low_b, 'max10diff', max(d))
print('first a', [hex(v) for v in x[:8]]); print('first b', [hex(v) for v in y[:8]])
bad = [i for i in range(n) if x[i] >> 6 != y[i] >> 6][:5]
print('first diffs at', bad, [(hex(x[i]), hex(y[i])) for i in bad])
