#!/usr/bin/env python3
"""V1: H.264 test vectors for the NVDEC driver (host test + nvdec_play on
the target). Needs PyAV (pip install av; its wheels bundle libx264).

For each preset writes, under OUT (default tests/nvdec_h264/vectors):
  NAME.h264  Annex B elementary stream (SPS/PPS in band)
  NAME.pts   one line per access unit in decode order: its display index
  NAME.md5   one line per frame in display order: md5 of the cropped NV12
             image (Y rows, then interleaved UV rows), from libavcodec's
             software decoder -- the reference for the hardware output.
"""
import argparse
from fractions import Fraction
import hashlib
import os
import sys

import av
import numpy as np

PRESETS = {
    # baseline, CAVLC, no B frames (x264 then uses POC type 2), 4 slices, 2 IDRs
    'base_ip': dict(size=(176, 144), frames=24, profile='baseline',
                    params='bframes=0:keyint=12:min-keyint=12:scenecut=0:slices=4:ref=2'),
    # main, CABAC, B pyramid (MMCO ops, reordering), 3 refs
    'main_b': dict(size=(176, 144), frames=40, profile='main',
                   params='bframes=3:b-pyramid=normal:ref=3:keyint=30:min-keyint=30:scenecut=0'),
    # high, 8x8 transform, JVT scaling lists, weighted prediction, cropped size
    'high_cqm': dict(size=(200, 120), frames=30, profile='high',
                     params='bframes=2:ref=4:cqm=jvt:weightp=2:weightb=1:keyint=60:scenecut=0'),
    # interlaced (x264 MBAFF, top field first): frame_mbs_only 0, per-field
    # POC (delta_pic_order_cnt_bottom), 160 coded rows cropped to 144
    'mbaff_tff': dict(size=(176, 144), frames=30, profile='high',
                      params='interlaced=1:tff=1:bframes=2:ref=3:keyint=30:scenecut=0'),
    # 1080p high, one second (target-side smoke test at a real size)
    'hd_high': dict(size=(1920, 1080), frames=30, profile='high',
                    params='bframes=3:b-pyramid=normal:ref=4:keyint=30:scenecut=0'),
    # HEVC (x265 defaults: SAO, AMP, WPP, TMVP, strong intra smoothing): I/P only, 2 IDRs
    'hevc_ip': dict(codec='hevc', size=(176, 144), frames=24, profile='main',
                    params='bframes=0:keyint=12:min-keyint=12:scenecut=0:ref=2'),
    # B pyramid, 3 refs, open GOP (CRA), coded 200x120 cropped to 200x116 (conformance window)
    'hevc_b': dict(codec='hevc', size=(200, 116), frames=40, profile='main',
                   params='bframes=3:b-pyramid=1:ref=3:keyint=30:min-keyint=30:scenecut=0:open-gop=1'),
    'hevc_hd': dict(codec='hevc', size=(1920, 1080), frames=30, profile='main',
                    params='bframes=3:ref=4:keyint=30:scenecut=0'),
}


def frame_image(i, w, h):
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 3), np.uint8)
    img[..., 0] = (x * 3 + i * 7) & 255
    img[..., 1] = (y * 2 + i * 5) & 255
    img[..., 2] = ((x // 16 + y // 16 + i) & 1) * 200
    # a moving block so motion search has something to do
    bx, by = (i * 5) % max(w - 32, 1), (i * 3) % max(h - 32, 1)
    img[by:by + 32, bx:bx + 32] = (255, 255, 0)
    return img


def nv12_md5(frame, w, h):
    yuv = frame.to_ndarray(format='yuv420p')   # (h*3/2, w)
    ypl = yuv[:h, :w]
    u = yuv[h:h + h // 4, :].reshape(h // 2, w // 2)
    v = yuv[h + h // 4:, :].reshape(h // 2, w // 2)
    uv = np.empty((h // 2, w), np.uint8)
    uv[:, 0::2] = u
    uv[:, 1::2] = v
    return hashlib.md5(ypl.tobytes() + uv.tobytes()).hexdigest()


def make(name, p, out):
    w, h = p['size']
    hevc = p.get('codec') == 'hevc'
    enc = av.CodecContext.create('libx265' if hevc else 'libx264', 'w')
    enc.width, enc.height = w, h
    enc.pix_fmt = 'yuv420p'
    enc.time_base = Fraction(1, 30)
    enc.framerate = Fraction(30, 1)
    enc.options = {'profile': p['profile'], ('x265-params' if hevc else 'x264-params'): p['params'] + ':crf=18',
                   'preset': 'medium'}
    packets = []
    for i in range(p['frames']):
        vf = av.VideoFrame.from_ndarray(frame_image(i, w, h), format='rgb24').reformat(format='yuv420p')
        vf.pts = i
        packets += enc.encode(vf)
    packets += enc.encode(None)
    data = b''.join(bytes(pk) for pk in packets)
    with open(os.path.join(out, name + ('.h265' if hevc else '.h264')), 'wb') as f:
        f.write(data)
    with open(os.path.join(out, name + '.pts'), 'w', newline='\n') as f:   # LF on Windows too
        f.writelines(f'{pk.pts}\n' for pk in packets)
    dec = av.CodecContext.create('hevc' if hevc else 'h264', 'r')
    md5 = []
    for pk in dec.parse(data) + dec.parse(None):
        for fr in dec.decode(pk):
            md5.append(nv12_md5(fr, w, h))
    for fr in dec.decode(None):
        md5.append(nv12_md5(fr, w, h))
    with open(os.path.join(out, name + '.md5'), 'w', newline='\n') as f:
        f.writelines(m + '\n' for m in md5)
    print(f'{name}: {w}x{h}, {len(packets)} AUs, {len(data)} bytes, {len(md5)} reference frames')
    if len(md5) != p['frames'] or len(packets) != p['frames']:
        sys.exit(f'{name}: frame count mismatch')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', '..',
                                                  'tests', 'nvdec_h264', 'vectors'))
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    # hd_high (2 MB) only on request: gen_vectors.py hd_high
    for n in a.names or [k for k in PRESETS if k not in ('hd_high', 'hevc_hd') and 'codec' not in PRESETS[k]]:
        make(n, PRESETS[n], a.out)


if __name__ == '__main__':
    main()
