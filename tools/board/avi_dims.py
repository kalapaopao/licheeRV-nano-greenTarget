#!/usr/bin/env python3
"""读 MJPEG-AVI：AVI 头画幅 + 第一帧 JPEG 的 SOF 真实宽高。

用法: avi_dims.py <video.avi>
"""
import struct
import sys

p = sys.argv[1]
d = open(p, "rb").read(1 << 20)   # 只读前 1MB 足够（头 + 首帧）

# ---- AVI avih 画幅 ----
i = d.find(b"avih")
if i < 0:
    print("avih not found")
else:
    w = struct.unpack_from("<I", d, i + 40)[0]
    h = struct.unpack_from("<I", d, i + 44)[0]
    print("AVI avih 画幅 : %d x %d" % (w, h))

# ---- 第一帧 JPEG 的 SOF ----
m = d.find(b"\xFF\xD8")
if m < 0:
    print("no JPEG")
    sys.exit(0)
q = m + 2
jw = jh = None
while q + 9 < len(d):
    if d[q] != 0xFF:
        q += 1
        continue
    mk = d[q + 1]
    if mk in (0xC0, 0xC1, 0xC2, 0xC3):
        jh = struct.unpack_from(">H", d, q + 5)[0]
        jw = struct.unpack_from(">H", d, q + 7)[0]
        print("首帧 JPEG SOF  : %d x %d   (marker FF%02X)" % (jw, jh, mk))
        break
    if mk in (0xD8, 0xD9) or 0xD0 <= mk <= 0xD7:
        q += 2
        continue
    seg = struct.unpack_from(">H", d, q + 2)[0]
    q += 2 + seg
if jw is None:
    print("SOF not found")

# ---- 帧大小统计（前 20 个 chunk）----
movi = d.find(b"movi") + 4
q = movi
sizes = []
while q + 8 <= len(d) and len(sizes) < 20:
    fc = d[q:q + 4]
    if fc in (b"idx1", b"LIST", b"RIFF"):
        break
    sz = struct.unpack_from("<I", d, q + 4)[0]
    if sz == 0:
        break
    if fc in (b"00dc", b"00db"):
        sizes.append(sz)
    q += 8 + sz + (sz & 1)
if sizes:
    print("前 %d 帧 JPEG  : 平均 %.1f KB  (min %.1f / max %.1f)"
          % (len(sizes), sum(sizes) / len(sizes) / 1024,
             min(sizes) / 1024, max(sizes) / 1024))
