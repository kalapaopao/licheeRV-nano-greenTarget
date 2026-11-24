#!/usr/bin/env python3
"""从 MJPEG-AVI 里抽出第 N 帧的原始 JPEG（板上无 PIL，只切字节）。
用法: dump_avi_frame.py <avi> <frame_index> <out.jpg>
"""
import struct
import sys

src, idx, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3]
d = open(src, "rb").read()
movi = d.find(b"movi") + 4
p = movi
n = 0
while p + 8 <= len(d):
    fc = d[p:p + 4]
    if fc == b"idx1" or fc == b"LIST" or fc == b"RIFF":
        break
    sz = struct.unpack_from("<I", d, p + 4)[0]
    if sz == 0 or p + 8 + sz > len(d):
        break
    if fc in (b"00dc", b"00db") and sz >= 4 and d[p + 8] == 0xFF and d[p + 9] == 0xD8:
        if n == idx:
            open(dst, "wb").write(d[p + 8:p + 8 + sz])
            print("wrote %s from chunk %d (%d bytes)" % (dst, n, sz))
            sys.exit(0)
        n += 1
    p += 8 + sz + (sz & 1)
print("frame %d not found (total %d)" % (idx, n))
sys.exit(1)
