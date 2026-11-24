#!/usr/bin/env python3
"""统计单个会话在起飞瞬间的视频空档（纯标准库）。用法: cg1.py <会话目录>"""
import csv
import os
import sys


def load(path, cols):
    out = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                out.append({c: float(row[c]) for c in cols})
            except (KeyError, ValueError):
                continue
    return out


sd = sys.argv[1]
fr = load(os.path.join(sd, "frames.csv"), ["capture_us", "mcu_state"])
vf = load(os.path.join(sd, "video_frames.csv"), ["capture_us"])
if not fr or not vf:
    print("    数据不足")
    sys.exit(0)

t0 = fr[0]["capture_us"]
t = [(r["capture_us"] - t0) / 1e6 for r in fr]
vt = [(r["capture_us"] - t0) / 1e6 for r in vf]

t_fl = None
for i in range(1, len(fr)):
    if int(fr[i]["mcu_state"]) == 1 and int(fr[i - 1]["mcu_state"]) != 1:
        t_fl = t[i]
        break

if t_fl is None:
    print("    没有 FLIGHT 边沿（假电控可能没进飞行）")
    sys.exit(0)

print("    起飞 t=%.3fs，数据 %.2fs / %d 帧，视频 %d 帧"
      % (t_fl, t[-1], len(fr), len(vf)))

# 起飞前后 ±0.2s 内的最大视频空档
near = [(vt[i], vt[i + 1], vt[i + 1] - vt[i])
        for i in range(len(vt) - 1) if abs(vt[i] - t_fl) < 0.25]
if near:
    a, b, d = max(near, key=lambda x: x[2])
    verdict = "❌ 有起飞空档" if d > 0.3 else "✅ 无起飞空档"
    print("    起飞处空档 %.3fs (%.3f -> %.3f)  %s" % (d, a, b, verdict))
else:
    print("    起飞附近没有视频帧")

row = []
for off in range(-1, 3):
    a, b = t_fl + off, t_fl + off + 1
    nc = sum(1 for x in t if a <= x < b)
    nv = sum(1 for x in vt if a <= x < b)
    row.append("%+d:%d/%d" % (off, nv, nc))
print("    每秒(视频/相机): " + "  ".join(row))
