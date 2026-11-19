#!/usr/bin/env python3
"""Turn the video track of a real MP4 (H.264 or HEVC) into an NVDEC test vector, same layout as
gen_vectors.py: NAME.h264/.h265 (Annex B), NAME.pts (display index per access unit, decode order),
NAME.md5 (libavcodec reference, cropped NV12, display order). Usage: mp4_to_vector.py in.mp4 OUTDIR NAME [frames]
Needs PyAV (pip install av)."""
import os
import sys

import av

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_vectors import nv12_md5  # noqa: E402


def main():
    src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 30
    inp = av.open(src)
    st = inp.streams.video[0]
    codec = st.codec_context.name   # 'h264' or 'hevc'
    bsf = av.BitStreamFilterContext('h264_mp4toannexb' if codec == 'h264' else 'hevc_mp4toannexb', st)
    packets = []
    for pkt in inp.demux(st):
        if pkt.size == 0:
            continue
        for p in bsf.filter(pkt):
            packets.append(p)
        if len(packets) >= limit:
            break
    packets = packets[:limit]
    # display index per access unit = rank of its pts
    order = sorted(range(len(packets)), key=lambda i: packets[i].pts)
    rank = {i: r for r, i in enumerate(order)}
    ext = '.h264' if codec == 'h264' else '.h265'
    with open(os.path.join(out, name + ext), 'wb') as f:
        for p in packets:
            f.write(bytes(p))
    with open(os.path.join(out, name + '.pts'), 'w') as f:
        f.write(''.join(f'{rank[i]}\n' for i in range(len(packets))))
    dec = av.CodecContext.create(codec, 'r')
    frames = []
    for p in packets:
        for fr in dec.decode(av.Packet(bytes(p))):
            frames.append(fr)
    for fr in dec.decode(None):
        frames.append(fr)
    with open(os.path.join(out, name + '.md5'), 'w') as f:
        for fr in frames:
            f.write(nv12_md5(fr, fr.width, fr.height) + '\n')
    print(f'{name}: {codec} {len(packets)} access units, {len(frames)} frames, {frames[0].width}x{frames[0].height}')


if __name__ == '__main__':
    main()
