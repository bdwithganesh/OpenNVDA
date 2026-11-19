# Self-check for nvda-smi with fake ioreg data (runs anywhere: python3 test_nvda_smi.py).
import importlib.machinery, importlib.util, io, os, struct, contextlib
here = os.path.dirname(os.path.abspath(__file__))
loader = importlib.machinery.SourceFileLoader('smi', os.path.join(here, 'nvda-smi'))
smi = importlib.util.module_from_spec(importlib.util.spec_from_loader('smi', loader)); loader.exec_module(smi)
G = 'NVGspControl-'
fake = {G + 'native615-ready': True, G + 'gpu-temp-mC': 41000, G + 'perf-pstate': 0,
        G + 'clk615': struct.pack('<6I', 0, 3, 2550000, 2610000, 2550000, 11201000),
        G + 'mem-vram-bytes': 512 << 20, G + 'static-fb-bytes': 16 << 30, G + 'dp-display': 0x2000,
        G + 'dp-protocol': 9, G + 'display615-connect-mask': 0x2000 | 0x100, G + 'display615-fb-width': 3840,
        G + 'display615-fb-height': 2160, G + 'display615-copy-on': True, G + 'hotplug-poll-mask': 0x2000,
        G + 'boot-timeline': 'start@0 gmc-init-ready@2300 native615-ready@3400 ',
        G + 'native615-stage-ms': struct.pack('<16I', *([0, 5, 40, 300] + [0] * 12))}
smi.run = lambda *c: b'  1 0 0 0 0 org.local.macosdevicelab.NVGspControl (0.180.199) UUID\n'
smi.accel = lambda: {'NVAccelerator-flip-ce': 120}
out = io.StringIO()
with contextlib.redirect_stdout(out):
    smi.summary(fake); smi.displays(fake); smi.boot(fake); smi.query(fake, 'dp-')
t = out.getvalue()
for want in ('0.180.199', '41C   P0', 'GPC 2550 MHz (max 2610 MHz)', 'DRAM 11201 MHz', 'VRAM  512 MiB / 16384 MiB',
             'DP-0', '3840x2160', '<- driven (DP-B)', 'HDMI bring-up is not implemented', 'native615-ready',
             '3:300', 'NVGspControl-dp-display'):
    assert want in t, (want, t)
assert all(len(l) == 80 for l in t.splitlines()[1:8]), [len(l) for l in t.splitlines()[1:8]]
print('nvda-smi self-check OK'); print(t)
