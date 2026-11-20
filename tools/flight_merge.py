#!/usr/bin/env python3
"""把一次飞行记录（frames.csv + mcu.csv）合成「一帧一行」的表，供图表使用。

为什么需要合并：视觉和电控是两条独立的数据流（80Hz 帧 / 100Hz 电控帧，协议里
电控帧没有时间戳、也不回显视觉序号，见《视觉串口通信协议》§4.5），所以板端各记
各的、各带 CLOCK_MONOTONIC 时间戳。这里按「帧的采集时刻 → 最近一帧已收到的电控
数据」做 join（不插值、不外推），并把两个流的相对时间轴对齐到第一帧。

用法：
    python flight_merge.py logs/sess_000123s_900_001
    python flight_merge.py <session> --out merged.csv
    python flight_merge.py <session> --xlsx merged.xlsx     # 需要 pandas + openpyxl

输出：
    merged.csv   每帧一行：原始帧列 + 该帧时刻的最近电控状态 + t_s（相对时间）+ mcu_age_us
    终端摘要     时长 / 帧率 / STATE 时间线 / 检出率 / 角度与距离范围

只用标准库（板子上也能跑）；--xlsx 才需要 pandas/openpyxl。
"""
import argparse
import bisect
import csv
import os
import sys

STATE_NAME = {0: "STATIC", 1: "FLIGHT", 2: "STOP"}


def read_csv(path):
    with open(path, "r", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def fnum(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def inum(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def merge(session):
    frames = read_csv(os.path.join(session, "frames.csv"))
    mcu = read_csv(os.path.join(session, "mcu.csv"))
    if not frames:
        raise SystemExit("frames.csv 是空的：这次会话没有帧数据")

    mcu.sort(key=lambda r: inum(r["rx_us"]))
    mcu_us = [inum(r["rx_us"]) for r in mcu]
    t0 = inum(frames[0]["capture_us"])

    rows = []
    for fr in frames:
        cap = inum(fr["capture_us"])
        i = bisect.bisect_right(mcu_us, cap) - 1   # 最近一帧「已经收到」的电控数据
        m = mcu[i] if i >= 0 else None
        row = dict(fr)
        row["t_s"] = "%.6f" % ((cap - t0) / 1e6)
        if m is not None:
            row["mcu_seq"] = m["seq"]
            row["mcu_state"] = m["state"]
            row["mcu_flags"] = m["flags"]
            row["mcu_roll_deg"] = m["roll"]
            row["mcu_pitch_deg"] = m["pitch"]
            row["mcu_yaw_deg"] = m["yaw"]
            row["mcu_gx_dps"] = m["gx"]
            row["mcu_gy_dps"] = m["gy"]
            row["mcu_gz_dps"] = m["gz"]
            row["mcu_age_us"] = str(cap - inum(m["rx_us"]))
        else:
            for k in ("mcu_seq", "mcu_state", "mcu_flags", "mcu_roll_deg", "mcu_pitch_deg",
                      "mcu_yaw_deg", "mcu_gx_dps", "mcu_gy_dps", "mcu_gz_dps"):
                row[k] = ""
            row["mcu_age_us"] = ""
        rows.append(row)
    return frames, mcu, rows, t0


def summarize(session, frames, mcu, rows, t0):
    dur = (inum(frames[-1]["capture_us"]) - t0) / 1e6
    fps = len(frames) / dur if dur > 0 else 0.0
    print("会话 %s" % os.path.basename(os.path.abspath(session)))
    print("  时长       %.2f s，帧 %d 行，平均 %.2f fps" % (dur, len(frames), fps))
    print("  电控帧     %d 行（%.1f 帧/秒）" % (len(mcu), len(mcu) / dur if dur > 0 else 0.0))

    # STATE 时间线（只在变化时打印）
    timeline = []
    last = None
    for m in mcu:
        st = inum(m["state"], -1)
        if st != last:
            timeline.append(((inum(m["rx_us"]) - t0) / 1e6, st))
            last = st
    if timeline:
        parts = ["%+.3fs→%s" % (t, STATE_NAME.get(s, str(s))) for t, s in timeline]
        print("  STATE      " + "  ".join(parts))
    flight = [t for t, s in timeline if s == 1]
    if flight:
        start = flight[0]
        stop = None
        for t, s in timeline:
            if t >= start and s != 1:
                stop = t
                break
        print("  飞行窗口   %.3f s → %s" % (start, "%.3f s（%.3fs）" % (stop, stop - start)
                                           if stop is not None else "（没收到 STOP）"))

    det = sum(1 for r in rows if r.get("detected") == "1")
    print("  检出帧     %d / %d（%.1f%%）" % (det, len(rows), 100.0 * det / len(rows)))

    def rng(col):
        vals = [fnum(r[col]) for r in rows if r.get(col) not in (None, "")]
        return (min(vals), max(vals)) if vals else (0.0, 0.0)

    for col, unit in (("yaw_deg", "°"), ("pitch_deg", "°"), ("dist_m", "m")):
        lo, hi = rng(col)
        print("  %-10s %8.3f .. %8.3f %s" % (col, lo, hi, unit))

    ages = [inum(r["mcu_age_us"]) for r in rows if r.get("mcu_age_us") not in ("", None)]
    if ages:
        ages.sort()
        p50 = ages[len(ages) // 2] / 1000.0
        p99 = ages[min(len(ages) - 1, int(len(ages) * 0.99))] / 1000.0
        print("  电控数据年龄：中位 %.1f ms，p99 %.1f ms（帧采集时刻距最近电控帧）"
              % (p50, p99))


def write_csv(path, rows):
    if not rows:
        return
    cols = list(rows[0].keys())
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)


def write_xlsx(path, rows):
    try:
        import pandas as pd
        from openpyxl.chart import LineChart, Reference
    except ImportError as exc:
        print("跳过 --xlsx：%s（需要 pandas + openpyxl）" % exc)
        return False

    df = pd.DataFrame(rows)
    for col in ("t_s", "yaw_deg", "pitch_deg", "vyaw_dps", "vpitch_dps", "dist_m"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    with pd.ExcelWriter(path, engine="openpyxl") as xw:
        df.to_excel(xw, sheet_name="frames", index=False)
        ws = xw.sheets["frames"]
        t_col = list(df.columns).index("t_s") + 1
        chart = LineChart()
        chart.title = "yaw / pitch / dist vs t"
        chart.y_axis.title = "deg / m"
        chart.x_axis.title = "t (s)"
        n = len(df) + 1
        for name in ("yaw_deg", "pitch_deg", "dist_m"):
            if name in df.columns:
                idx = list(df.columns).index(name) + 1
                chart.add_data(Reference(ws, min_col=idx, min_row=1, max_row=n), titles_from_data=True)
        chart.set_categories(Reference(ws, min_col=t_col, min_row=2, max_row=n))
        ws.add_chart(chart, "A" + str(n + 3))
    print("已写出 %s（frames 表 + 一张折线图）" % path)
    return True


def main():
    # Windows 控制台/PowerShell 默认按 UTF-8 解码子进程输出；Python 在 cp936 控制台上
    # 会按本地编码写中文，结果就是一堆乱码。这里统一强制 UTF-8（失败就退回默认）。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description="合并一次飞行记录的帧流与电控流")
    ap.add_argument("session", help="会话目录（含 frames.csv / mcu.csv）")
    ap.add_argument("--out", default=None, help="输出 CSV（默认写到会话目录/merged.csv）")
    ap.add_argument("--xlsx", default=None, help="可选：另存一份带折线图的 xlsx")
    args = ap.parse_args()

    if not os.path.isdir(args.session):
        raise SystemExit("不是目录：%s" % args.session)

    frames, mcu, rows, t0 = merge(args.session)
    summarize(args.session, frames, mcu, rows, t0)
    out = args.out or os.path.join(args.session, "merged.csv")
    write_csv(out, rows)
    print("已写出 %s（%d 行 × %d 列）" % (out, len(rows), len(rows[0])))
    if args.xlsx:
        write_xlsx(args.xlsx, rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
