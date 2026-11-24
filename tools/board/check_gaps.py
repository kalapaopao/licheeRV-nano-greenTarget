#!/usr/bin/env python3
"""纯标准库：统计每个会话「起飞瞬间」的视频空档。"""
import csv
import os
import glob

def load(path, cols):
    out = []
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                out.append({c: float(row[c]) for c in cols})
            except (KeyError, ValueError):
                continue
    return out

for sd in sorted(glob.glob("/code/fly_LicheeRV/record/sess_*"), reverse=True):
    fp = os.path.join(sd, "frames.csv")
    vp = os.path.join(sd, "video_frames.csv")
    if not (os.path.exists(fp) and os.path.exists(vp)):
        continue
    fr = load(fp, ["capture_us", "mcu_state", "frame_id"])
    vf = load(vp, ["capture_us", "frame_id"])
    if not fr or not vf:
        continue
    t0 = fr[0]["capture_us"]
    t = [(r["capture_us"] - t0) / 1e6 for r in fr]
    vt = [(r["capture_us"] - t0) / 1e6 for r in vf]

    # 起飞时刻 = STATE 0 -> 1
    t_fl = None
    for i in range(1, len(fr)):
        if int(fr[i]["mcu_state"]) == 1 and int(fr[i - 1]["mcu_state"]) != 1:
            t_fl = t[i]
            break

    print("=== %s ===" % os.path.basename(sd))
    print("  数据 %.2fs / %d 帧，视频 %d 帧，起飞 %s" %
          (t[-1], len(fr), len(vf), ("%.2fs" % t_fl) if t_fl else "无 FLIGHT 边沿"))
    # 最大空档
    g = [(vt[i], vt[i + 1], vt[i + 1] - vt[i]) for i in range(len(vt) - 1)]
    g.sort(key=lambda x: -x[2])
    for a, b, d in g[:3]:
        near = ""
        if t_fl is not None and abs(a - t_fl) < 0.2:
            near = "  ★紧邻起飞"
        print("    空档 %.3fs  (%.3f -> %.3f)%s" % (d, a, b, near))
    # 起飞后 3 秒覆盖率
    if t_fl is not None:
        row = []
        for off in range(-1, 4):
            a, b = t_fl + off, t_fl + off + 1
            nc = sum(1 for x in t if a <= x < b)
            nv = sum(1 for x in vt if a <= x < b)
            row.append("%+d:%d/%d" % (off, nv, nc))
        print("    起飞前后每秒(视频/相机): " + "  ".join(row))
    print()

print("说明: 空档 0.2~2.5s 且紧邻起飞 = 预录 flush 阻塞编码线程（旧问题）")
