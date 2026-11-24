#!/usr/bin/env python3
"""完整性分析：判断一次飞行的 CSV 与视频是否完整。用法: cpg.py <会话目录>"""
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
fr = load(os.path.join(sd, "frames.csv"), ["capture_us", "mcu_state", "frame_id"])
vf = load(os.path.join(sd, "video_frames.csv"), ["capture_us", "frame_id"])
if not fr:
    print("  ❌ frames.csv 为空")
    sys.exit(1)

t0 = fr[0]["capture_us"]
t = [(r["capture_us"] - t0) / 1e6 for r in fr]
vt = [(r["capture_us"] - t0) / 1e6 for r in vf]

# 状态边沿
edges = []
prev = int(fr[0]["mcu_state"])
for i in range(1, len(fr)):
    s = int(fr[i]["mcu_state"])
    if s != prev:
        edges.append((t[i], prev, s))
        prev = s

print("  数据跨度 %.2f s，%d 帧" % (t[-1], len(fr)))
print("  STATE 边沿：", " ".join("%.2fs %s->%s" % (a, b, c) for a, b, c in edges) or "无")

t_fl = next((a for a, b, c in edges if c == 1), None)
t_sp = next((a for a, b, c in edges if c == 2), None)

if t_fl is None:
    print("  ❌ 没有起飞边沿")
    sys.exit(1)
print("  起飞 %.2fs" % t_fl)
if t_sp is not None:
    print("  落地 %.2fs  飞行时长 %.2f s" % (t_sp, t_sp - t_fl))

# 帧间隔（判 CSV 有无洞）
d = [(fr[i]["capture_us"] - fr[i - 1]["capture_us"]) / 1e3 for i in range(1, len(fr))]
d.sort()
med = d[len(d) // 2]
print("  帧间隔中位 %.1f ms（≈%.1f fps），最大 %.1f ms" % (med, 1000.0 / med, d[-1]))
big = [x for x in d if x > med * 4]
print("  超过 4 倍中位的间隔：%d 个 %s" % (len(big), ("前几个 %.0fms" % big[-1]) if big else ""))

# 视频覆盖
print()
print("  视频帧 %d 个" % len(vf))
if vf:
    print("  视频首帧 t=%.2fs  末帧 t=%.2fs" % (vt[0], vt[-1]))
    if t_sp is not None:
        cover = sum(1 for x in vt if t_fl <= x <= t_sp)
        need = sum(1 for x in t if t_fl <= x <= t_sp)
        print("  飞行段覆盖：视频 %d / 相机 %d = %.1f%%" %
              (cover, need, 100.0 * cover / need if need else 0))
    # 起飞处空档
    near = [(vt[i], vt[i + 1], vt[i + 1] - vt[i]) for i in range(len(vt) - 1)
            if abs(vt[i] - t_fl) < 0.25]
    if near:
        a, b, g = max(near, key=lambda x: x[2])
        print("  起飞处空档 %.3fs  %s" % (g, "✅" if g < 0.1 else "❌"))
    # 全片最大空档
    gs = [(vt[i], vt[i + 1], vt[i + 1] - vt[i]) for i in range(len(vt) - 1)]
    if gs:
        a, b, g = max(gs, key=lambda x: x[2])
        print("  全片最大空档 %.3fs (%.2f->%.2f)" % (g, a, b))

# 结论
print()
ok = True
if t_sp is None:
    print("  ⚠ 没等到落地（STOP），会话可能被 max_seconds 或退出截断")
    ok = False
if vf and t_sp is not None:
    need = sum(1 for x in t if t_fl <= x <= t_sp)
    cover = sum(1 for x in vt if t_fl <= x <= t_sp)
    if need and cover / need < 0.95:
        print("  ⚠ 飞行段视频覆盖不足 95%%（丢帧）")
        ok = False
print("  " + ("✅ 飞行段数据与视频完整" if ok else "⚠ 存在缺口，见上"))
