#!/usr/bin/env python3
"""验证「视频帧 → frame_id → 数据行」这条同步链是否精确。

三件事：
  1. 转码后的 mp4 第 i 帧 == 原 AVI 第 i 帧（逐帧顺序没被 ffmpeg 改动）
  2. video_frames.csv 的 avi_index 连续、frame_id 单调，且能落到 frames.csv 的某一行
  3. 生成的 index.html 里内嵌数据自洽（长度对齐、无 NaN、映射严格递增）

用法：
  python tools/verify_flight_sync.py record/sess_454s_395_001
退出码 0 = 全部通过。
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pandas as pd

FFMPEG = r"E:\study\SOP_train_model_resource\sop_installment\.ffmpeg\ffmpeg-9.0.2-essentials_build\bin\ffmpeg.exe"
FFPROBE = r"E:\study\SOP_train_model_resource\sop_installment\.ffmpeg\ffmpeg-9.0.2-essentials_build\bin\ffprobe.exe"

fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    print(("  ✅ " if cond else "  ❌ ") + msg)
    if not cond:
        fails.append(msg)


def count_frames(p: Path) -> int:
    out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                          "-count_frames", "-show_entries", "stream=nb_read_frames",
                          "-of", "csv=p=0", str(p)], capture_output=True, text=True).stdout
    return int(out.strip() or 0)


def probe_fps(p: Path) -> float:
    out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=avg_frame_rate", "-of", "csv=p=0", str(p)],
                         capture_output=True, text=True).stdout.strip()
    a, b = out.split("/")
    return float(a) / float(b)


def grab(src: Path, n: int, dst: Path) -> bool:
    r = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
                        "-vf", f"select=eq(n\\,{n})", "-fps_mode", "passthrough",
                        "-frames:v", "1", str(dst)], capture_output=True)
    return r.returncode == 0 and dst.exists()


def main() -> int:
    S = Path(sys.argv[1] if len(sys.argv) > 1 else "record")
    if not (S / "frames.csv").exists():
        subs = sorted([p for p in S.iterdir() if p.is_dir() and (p / "frames.csv").exists()],
                      key=lambda p: (p / "frames.csv").stat().st_mtime, reverse=True)
        if not subs:
            sys.exit(f"{S} 下没有会话")
        S = subs[0]
    print(f"会话: {S.name}\n")

    frames = pd.read_csv(S / "frames.csv")
    vf = pd.read_csv(S / "video_frames.csv")
    avi, mp4 = S / "video.avi", S / "web" / "video.mp4"

    # ---------- 1. 转码是否保持逐帧顺序 ----------
    print("1) 转码帧顺序（mp4 第 i 帧 vs AVI 第 i 帧）")
    if not mp4.exists():
        print("  ⚠ 没找到 web/video.mp4，跳过（先跑 flight_sync_player.py）")
    elif not avi.exists():
        print("  ⚠ 没找到 video.avi，跳过")
    else:
        na, nm = count_frames(avi), count_frames(mp4)
        check(na == nm, f"帧数一致：AVI {na} / MP4 {nm}")
        from PIL import Image
        tmp = Path(tempfile.mkdtemp(prefix="vsync_"))
        worst = 1.0
        for n in [0, na // 4, na // 2, (3 * na) // 4, na - 2]:
            a, b = tmp / f"a{n}.png", tmp / f"b{n}.png"
            if not (grab(avi, n, a) and grab(mp4, n, b)):
                check(False, f"第 {n} 帧抽取失败")
                continue
            ia = np.asarray(Image.open(a).convert("L"), float)
            ib = np.asarray(Image.open(b).convert("L"), float)
            if ia.shape != ib.shape:
                check(False, f"第 {n} 帧尺寸不同")
                continue
            corr = float(np.corrcoef(ia.ravel(), ib.ravel())[0, 1])
            worst = min(worst, corr)
            print(f"     第 {n:5d} 帧  灰度相关 {corr:.5f}")
        check(worst > 0.99, f"所有采样帧一致（最差相关 {worst:.5f} > 0.99）")

    # ---------- 2. 映射表 ----------
    print("\n2) avi_index -> frame_id -> 数据行")
    a = vf["avi_index"].to_numpy(int)
    check(np.array_equal(a, np.arange(len(a))), f"avi_index 从 0 连续到 {len(a)-1}")
    check(bool(np.all(np.diff(vf["frame_id"].to_numpy(int)) > 0)), "frame_id 严格递增")
    fid0, fidN = int(frames["frame_id"].iloc[0]), int(frames["frame_id"].iloc[-1])
    inside = ((vf["frame_id"] >= fid0) & (vf["frame_id"] <= fidN)).sum()
    tail = len(vf) - inside
    print(f"     frames.csv frame_id {fid0}..{fidN}（{len(frames)} 行）")
    print(f"     可落行 {inside} / {len(vf)}，尾部无数据 {tail} 帧（会话收尾时写入，属正常）")
    check(inside >= len(vf) - 2, "绝大多数视频帧能落到数据行（尾部最多 2 帧例外）")
    # 采样反查
    ok = 0
    for i in [0, len(vf) // 3, len(vf) // 2, len(vf) - 3]:
        r = int(vf["frame_id"].iloc[i]) - fid0
        if 0 <= r < len(frames) and int(frames["frame_id"].iloc[r]) == int(vf["frame_id"].iloc[i]):
            ok += 1
    check(ok >= 3, f"采样反查 {ok}/4 命中")

    # ---------- 3. 页面自洽 ----------
    print("\n3) index.html 内嵌数据")
    html = S / "web" / "index.html"
    if not html.exists():
        print("  ⚠ 没有 index.html，跳过")
    else:
        s = html.read_text(encoding="utf-8")
        m = re.search(r"const D = (\{.*?\});\n", s, re.S)
        check(m is not None, "找到内嵌 JSON")
        if m:
            raw = m.group(1)
            check("NaN" not in raw and "Infinity" not in raw, "JSON 里没有 NaN/Infinity")
            D = json.loads(raw)
            nrow = len(D["t"])
            check(D["nAvi"] == len(D["rowOfAvi"]), f"rowOfAvi 长度 == nAvi（{D['nAvi']}）")
            bad = [k for k in ("st", "roll", "pitch", "yaw", "gx", "gy", "gz",
                               "age", "det", "hit") if len(D[k]) != nrow]
            check(not bad, f"各数据列长度都等于 {nrow}（异常列：{bad or '无'}）")
            vals = [v for v in D["rowOfAvi"] if v is not None]
            check(all(y > x for x, y in zip(vals, vals[1:])),
                  "rowOfAvi 严格递增（视频顺序与数据一致）")
            check(max(vals) < nrow, "rowOfAvi 不越界")
            check('src="video.mp4"' in s, "页面引用了同目录 video.mp4")

    print()
    if fails:
        print(f"❌ {len(fails)} 项未通过")
        return 1
    print("✅ 同步链验证全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
