#!/usr/bin/env python3
"""把一次飞行会话做成「视频 + 数据逐帧同步」的网页。

核心是**对齐方式**：不能按播放时间对齐。
  AVI 容器是恒定帧率，但录制时帧间隔是变化的（实测视频时刻与真实采集时刻
  相差中位 860ms、最大 1.6s），按时间对齐会错开几十帧。
  所以这里走另一条链，逐帧精确：
      浏览器 currentTime  ->  avi_index = floor(ct * fps)
                          ->  frame_id  = video_frames.csv[avi_index].frame_id
                          ->  数据行    = frames.csv[frame_id]
  实测这条链 10/10 命中（见 verify_sync.py）。

产物：
  <session>/web/index.html   自包含页面（数据内嵌）
  <session>/web/video.mp4    原视频转码（浏览器能播的 H.264）

用法：
  python tools/flight_sync_player.py record/sess_454s_395_001
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

FFMPEG_CANDIDATES = [
    r"E:\study\SOP_train_model_resource\sop_installment\.ffmpeg\ffmpeg-9.0.2-essentials_build\bin\ffmpeg.exe",
    "ffmpeg",
]

C = {
    "roll": "#2563eb", "pitch": "#059669", "yaw": "#d97706",
    "gx": "#db2777", "gy": "#0891b2", "gz": "#7c3aed",
    "age": "#0ea5e9", "det": "#8b5cf6", "fdt": "#f97316",
    "grid": "#eef1f5", "axis": "#cbd5e1", "muted": "#6b7280",
    "band_static": "#f1f3f6", "band_flight": "#e8f0fe", "band_stop": "#fff5e0",
}
STATE_NAME = {0: "STATIC", 1: "FLIGHT", 2: "STOP", -1: "无数据"}
STATE_COLOR = {0: "#64748b", 1: "#2563eb", 2: "#d97706", -1: "#94a3b8"}

W, ML, MR, MT, MB = 1160, 62, 18, 16, 26


def find_tool(name: str) -> str | None:
    for c in FFMPEG_CANDIDATES:
        p = c if os.sep in c else shutil.which(c)
        if not p:
            continue
        if os.path.basename(p).lower() != (name + ".exe"):
            cand = os.path.join(os.path.dirname(p), name + (".exe" if p.lower().endswith(".exe") else ""))
            p = cand if os.path.exists(cand) else p
        if os.path.exists(p):
            return p
    return None


def probe_fps(ffprobe: str, path: Path) -> tuple[float, int]:
    out = subprocess.run(
        [ffprobe, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=r_frame_rate,nb_frames,avg_frame_rate",
         "-of", "json", str(path)],
        capture_output=True, text=True, check=True).stdout
    st = json.loads(out)["streams"][0]
    r = st.get("avg_frame_rate") or st["r_frame_rate"]
    num, den = r.split("/")
    fps = float(num) / float(den)
    return fps, int(st.get("nb_frames") or 0)


def nice_ticks(lo, hi, target=5):
    if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
        return [lo]
    raw = (hi - lo) / max(1, target)
    mag = 10 ** np.floor(np.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            step = m * mag
            break
    else:
        step = 10 * mag
    v, out = np.ceil(lo / step) * step, []
    while v <= hi + step * 1e-9:
        out.append(round(float(v), 10))
        v += step
    return out


def f0(v, nd=1):
    return "—" if v is None or not np.isfinite(v) else f"{v:,.{nd}f}"


class Panel:
    def __init__(self, idx, title, unit, h, show_x, xhi):
        self.idx, self.title, self.unit, self.h = idx, title, unit, h
        self.show_x, self.xhi = show_x, xhi
        self.series = []

    def set_range(self, lo, hi, pad=0.08, floor=None):
        if floor is not None:
            lo = min(lo, floor)
        p = (hi - lo) * pad or 1.0
        self.ylo, self.yhi = lo - p, hi + p
        self.yticks = nice_ticks(self.ylo, self.yhi, 3)

    def px(self, x):
        return ML + (x / self.xhi) * (W - ML - MR)

    def py(self, y):
        return MT + (self.yhi - y) / (self.yhi - self.ylo) * (self.h - MT - MB)

    def add(self, name, color, xs, ys, width=1.4, opacity=1.0):
        self.series.append(dict(name=name, color=color, xs=np.asarray(xs, float),
                                ys=np.asarray(ys, float), width=width, opacity=opacity))

    def svg(self, bands):
        o = [f'<svg class="panel" id="p{self.idx}" viewBox="0 0 {W} {self.h}" '
             f'preserveAspectRatio="none" data-idx="{self.idx}">']
        for a, b, color in bands:
            x1, x2 = self.px(max(a, 0)), self.px(min(b, self.xhi))
            if x2 > x1 + .3:
                o.append(f'<rect x="{x1:.1f}" y="{MT}" width="{x2 - x1:.1f}" '
                         f'height="{self.h - MT - MB}" fill="{color}"/>')
        for v in self.yticks:
            y = self.py(v)
            if MT - 1 <= y <= self.h - MB + 1:
                o.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{W - MR}" y2="{y:.1f}" '
                         f'stroke="{C["grid"]}"/>')
                o.append(f'<text x="{ML - 8}" y="{y + 3.6:.1f}" text-anchor="end" '
                         f'class="yt">{f0(v, 0 if abs(v) >= 100 else 1)}</text>')
        if self.ylo < 0 < self.yhi:
            y0 = self.py(0)
            o.append(f'<line x1="{ML}" y1="{y0:.1f}" x2="{W - MR}" y2="{y0:.1f}" '
                     f'stroke="{C["axis"]}"/>')
        for s in self.series:
            pts, segs, seg = [], [], []
            for x, y in zip(s["xs"], s["ys"]):
                if not np.isfinite(y):
                    if len(seg) > 1:
                        segs.append(seg)
                    seg = []
                else:
                    seg.append(f"{self.px(x):.1f},{self.py(y):.1f}")
            if len(seg) > 1:
                segs.append(seg)
            for g in segs:
                o.append(f'<polyline points="{" ".join(g)}" fill="none" stroke="{s["color"]}" '
                         f'stroke-width="{s["width"]}" stroke-linejoin="round" '
                         f'stroke-linecap="round" opacity="{s["opacity"]}"/>')
        o.append(f'<line x1="{ML}" y1="{self.h - MB}" x2="{W - MR}" y2="{self.h - MB}" '
                 f'stroke="{C["axis"]}"/>')
        if self.show_x:
            for v in nice_ticks(0, self.xhi, 10):
                x = self.px(v)
                o.append(f'<line x1="{x:.1f}" y1="{self.h - MB}" x2="{x:.1f}" '
                         f'y2="{self.h - MB + 4}" stroke="{C["axis"]}"/>')
                o.append(f'<text x="{x:.1f}" y="{self.h - MB + 16}" text-anchor="middle" '
                         f'class="xt">{f0(v, 0)}</text>')
        o.append(f'<text x="{ML}" y="{MT + 10}" class="pt">{self.title}</text>')
        if self.unit:
            o.append(f'<text x="{W - MR}" y="{MT + 10}" text-anchor="end" class="pu">{self.unit}</text>')
        # 播放头
        o.append(f'<line class="head" x1="0" y1="{MT}" x2="0" y2="{self.h - MB}" '
                 f'stroke="#ef4444" stroke-width="1.4" opacity="0"/>')
        o.append(f'<rect x="{ML}" y="{MT}" width="{W - ML - MR}" '
                 f'height="{self.h - MT - MB}" fill="transparent" class="clicker"/>')
        o.append("</svg>")
        return "\n".join(o)


def build(session: Path, crf: int = 18) -> Path:
    ffmpeg, ffprobe = find_tool("ffmpeg"), find_tool("ffprobe")
    if not ffmpeg or not ffprobe:
        sys.exit("找不到 ffmpeg/ffprobe，无法转码")

    frames = pd.read_csv(session / "frames.csv")
    vf = pd.read_csv(session / "video_frames.csv")
    avi = session / "video.avi"
    if not avi.exists():
        sys.exit(f"找不到 {avi}")

    web = session / "web"
    web.mkdir(exist_ok=True)
    mp4 = web / "video.mp4"

    print(f"=== 转码 {avi.name} -> web/video.mp4 (H.264 crf={crf}) ===")
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(avi),
        "-vf", "scale=in_range=full:out_range=limited,format=yuv420p",
        "-c:v", "libx264", "-preset", "medium", "-crf", str(crf),
        "-pix_fmt", "yuv420p", "-color_range", "tv",
        "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-movflags", "+faststart", "-an", str(mp4)],
        check=True)
    fps, nfr = probe_fps(ffprobe, mp4)
    print(f"    转码后 {nfr} 帧 @ {fps:.4f} fps，{mp4.stat().st_size / 1e6:.1f} MB")
    if nfr and nfr < len(vf):
        print(f"    ⚠ 转码丢帧：视频 {nfr} 帧 < 侧车 {len(vf)} 行，后面的帧会错位！")
    elif nfr > len(vf):
        print(f"    注：视频比侧车多 {nfr - len(vf)} 帧 —— 是会话收尾那一刻写进 AVI、"
              f"但没来得及记进 video_frames.csv 的尾帧，页面上会显示「无对应数据」")

    # ---------- 数据整理 ----------
    t = (frames["capture_us"].to_numpy(float) - frames["capture_us"].iloc[0]) / 1e6
    xhi = float(t[-1])
    st = frames["mcu_state"].fillna(-1).to_numpy(int)
    fid0 = int(frames["frame_id"].iloc[0])

    # 视频帧 -> 数据行下标
    row_of_avi = (vf["frame_id"].to_numpy(int) - fid0)
    valid = (row_of_avi >= 0) & (row_of_avi < len(frames))
    print(f"    映射检查：{valid.sum()}/{len(valid)} 个视频帧能落到数据行")

    # 相位色带
    bands, prev, start = [], int(st[0]), 0.0
    for i in range(1, len(st)):
        if st[i] != prev:
            bands.append((start, float(t[i]), C["band_flight"] if prev == 1
                          else C["band_stop"] if prev == 2 else C["band_static"]))
            start, prev = float(t[i]), int(st[i])
    bands.append((start, xhi, C["band_flight"] if prev == 1
                  else C["band_stop"] if prev == 2 else C["band_static"]))

    dt_ms = np.diff(frames["capture_us"].to_numpy(float)) / 1e3

    panels = []
    def mk(idx, title, unit, series, h=150, show_x=False, floor=None):
        p = Panel(idx, title, unit, h, show_x, xhi)
        vals = np.concatenate([np.asarray(s[2], float) for s in series])
        vals = vals[np.isfinite(vals)]
        p.set_range(float(vals.min()), float(vals.max()), floor=floor)
        for nm, col, xs, ys, w, op in series:
            p.add(nm, col, xs, ys, w, op)
        panels.append(p)

    mk(0, "姿态角 滚转 / 俯仰 / 偏航", "deg", [
        ("滚转 roll", C["roll"], t, frames["mcu_roll"], 1.5, 1.0),
        ("俯仰 pitch", C["pitch"], t, frames["mcu_pitch"], 1.5, 1.0),
        ("偏航 yaw", C["yaw"], t, frames["mcu_yaw"], 1.2, .85)])
    mk(1, "角速度 gx / gy / gz", "deg/s", [
        ("gx", C["gx"], t, frames["mcu_gx"], 1.1, .95),
        ("gy", C["gy"], t, frames["mcu_gy"], 1.1, .95),
        ("gz", C["gz"], t, frames["mcu_gz"], 1.1, .95)])
    mk(2, "电控数据年龄", "ms", [("age", C["age"], t, frames["mcu_age_ms"], 1.2, 1.0)],
       h=110, floor=0)
    mk(3, "检测耗时", "ms", [("detect", C["det"], t, frames["detect_us"] / 1000.0, 1.0, 1.0)],
       h=110, floor=0)
    mk(4, "帧间隔（尖峰=丢帧）", "ms", [("dt", C["fdt"], t[1:], dt_ms, .9, 1.0)],
       h=104, show_x=True, floor=0)

    # ---------- 内嵌数据 ----------
    def col(name, nd=None):
        a = frames[name].to_numpy(float)
        out = []
        for v in a:
            if not np.isfinite(v):
                out.append(None)
            elif nd is None:
                out.append(int(v))
            else:
                out.append(round(float(v), nd))
        return out

    payload = {
        "fps": round(fps, 6),
        "fid0": fid0,
        # 以「视频容器里真实的帧数」为准，不用 min()：
        # AVI 尾部可能有一帧是会话收尾时写进去、但没记进侧车的（实测多 1 帧），
        # 那一帧在 rowOfAvi 里是 null，页面显示「无对应数据」而不是错位。
        "nAvi": int(nfr or len(vf)),
        "rowOfAvi": ([int(r) if v else None for r, v in zip(row_of_avi, valid)]
                     + [None] * max(0, int(nfr or 0) - len(vf))),
        "t": [round(float(v), 4) for v in t],
        "xhi": round(xhi, 4),
        "st": [STATE_NAME.get(int(v), "?") for v in st],
        "roll": col("mcu_roll", 2), "pitch": col("mcu_pitch", 2), "yaw": col("mcu_yaw", 2),
        "gx": col("mcu_gx", 1), "gy": col("mcu_gy", 1), "gz": col("mcu_gz", 1),
        "age": col("mcu_age_ms"),
        "det": [round(float(v) / 1000, 2) if np.isfinite(v) else None for v in frames["detect_us"]],
        "hit": col("detected"),
        "gxp": col("gx_px", 1), "gyp": col("gy_px", 1), "grp": col("gr_px", 1),
        "tyaw": col("yaw_deg", 1), "tpitch": col("pitch_deg", 1),
        "band": [[round(a, 3), round(b, 3), c] for a, b, c in bands],
    }

    legend = lambda p: "".join(
        f'<span class="lg"><i style="background:{s["color"]}"></i>{s["name"]}</span>'
        for s in p.series)

    fl = np.where((st == 1) & (np.r_[0, st[:-1]] != 1))[0]
    sp = np.where((st == 2) & (np.r_[0, st[:-1]] != 2))[0]
    t_fl = float(t[fl[0]]) if len(fl) else None
    t_sp = float(t[sp[0]]) if len(sp) else None

    fps_s = f"{fps:.2f}"
    n_avi = payload["nAvi"]

    html = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>逐帧同步 · {session.name}</title>
<style>
:root{{--bg:#f6f7f9;--card:#fff;--ink:#0f172a;--muted:#6b7280;--line:#e5e7eb}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);
 font:14px/1.55 "Segoe UI",system-ui,-apple-system,"Microsoft YaHei",sans-serif}}
.wrap{{max-width:1260px;margin:0 auto;padding:22px 20px 60px}}
h1{{font-size:19px;margin:0 0 3px}}
.sub{{color:var(--muted);font-size:12.5px;margin-bottom:16px}}
.card{{background:var(--card);border:1px solid var(--line);border-radius:14px;
 padding:16px 18px;box-shadow:0 1px 2px rgba(16,24,40,.04)}}
.grid{{display:grid;grid-template-columns:minmax(0,1.55fr) minmax(280px,.85fr);
 gap:16px;margin-bottom:16px}}
@media(max-width:900px){{.grid{{grid-template-columns:1fr}}}}
video{{width:100%;display:block;border-radius:10px;background:#0b0f14}}
.bar{{display:flex;align-items:center;gap:10px;margin-top:12px;flex-wrap:wrap}}
button{{font:inherit;padding:6px 13px;border-radius:9px;border:1px solid var(--line);
 background:#fff;color:var(--ink);cursor:pointer}}
button:hover{{background:#f3f4f6}}
button.primary{{background:#2563eb;border-color:#2563eb;color:#fff}}
button.primary:hover{{background:#1d4ed8}}
.seg{{display:inline-flex;border:1px solid var(--line);border-radius:9px;overflow:hidden}}
.seg button{{border:0;border-radius:0;padding:6px 11px}}
.seg button.on{{background:#e8f0fe;color:#1d4ed8;font-weight:600}}
.pill{{font-size:12px;color:var(--muted);margin-left:auto;font-variant-numeric:tabular-nums}}
.kv{{display:grid;grid-template-columns:auto 1fr;gap:7px 14px;font-size:13px;
 font-variant-numeric:tabular-nums}}
.kv dt{{color:var(--muted)}}
.kv dd{{margin:0;font-weight:600}}
.big{{font-size:15px}}
.badge{{display:inline-block;padding:1.5px 9px;border-radius:999px;color:#fff;
 font-size:12px;font-weight:600;letter-spacing:.3px}}
.hit{{color:#16a34a;font-weight:600}}
.panel{{width:100%;height:auto;display:block;overflow:visible}}
.yt,.xt{{font-size:10.5px;fill:var(--muted);font-variant-numeric:tabular-nums}}
.pt{{font-size:12px;fill:#374151;font-weight:600}}
.pu{{font-size:10.5px;fill:#9ca3af}}
.clicker{{cursor:pointer}}
.legend{{display:flex;gap:15px;padding:2px 0 10px;flex-wrap:wrap}}
.lg{{font-size:11.5px;color:#4b5563;display:inline-flex;align-items:center;gap:6px}}
.lg i{{width:12px;height:3px;border-radius:2px}}
.hint{{font-size:12px;color:var(--muted);margin:0 0 10px}}
</style></head><body><div class="wrap">
<h1>逐帧同步回放</h1>
<div class="sub">{session.name}　·　{fps_s} fps　·　{n_avi} 个视频帧　·　
对齐方式：视频帧号 → frame_id → 数据行（不按时间对齐）</div>

<div class="grid">
  <div class="card">
    <video id="v" src="video.mp4" controls preload="auto" playsinline></video>
    <div class="bar">
      <button id="prev" title="上一帧（←）">◀ 上一帧</button>
      <button id="play" class="primary">播放</button>
      <button id="next" title="下一帧（→）">下一帧 ▶</button>
      <span class="seg" id="spd">
        <button data-r="0.25">0.25×</button>
        <button data-r="0.5">0.5×</button>
        <button data-r="1" class="on">1×</button>
      </span>
      <span class="pill" id="pos">—</span>
    </div>
    <p class="hint" style="margin:12px 0 0">
      画面上角烧入的 <code>Fxxxxx</code> 就是 frame_id，可以和右边的数字直接对照。
      视频按恒定 {fps_s} fps 存放，录制时帧间隔有抖动，所以这里<b>不用播放时间对齐</b>，
      而是用「第几帧」去查数据。
    </p>
  </div>

  <div class="card">
    <div style="display:flex;align-items:center;gap:10px;margin-bottom:12px">
      <span style="font-weight:600">当前帧数据</span>
      <span class="badge" id="badge" style="background:#94a3b8">—</span>
    </div>
    <dl class="kv">
      <dt>frame_id</dt><dd class="big" id="k_fid">—</dd>
      <dt>视频帧号</dt><dd id="k_avi">—</dd>
      <dt>视频时刻</dt><dd id="k_vt">—</dd>
      <dt>采集时刻</dt><dd id="k_t">—</dd>
      <dt>偏差</dt><dd id="k_drift" style="font-weight:400;color:#6b7280">—</dd>
      <dt>滚转 / 俯仰 / 偏航</dt><dd id="k_rpy">—</dd>
      <dt>角速度 gx/gy/gz</dt><dd id="k_g">—</dd>
      <dt>电控年龄</dt><dd id="k_age">—</dd>
      <dt>检测耗时</dt><dd id="k_det">—</dd>
      <dt>检测目标</dt><dd id="k_hit">—</dd>
    </dl>
  </div>
</div>

<div class="card">
  <p class="hint">红色竖线 = 当前画面所在时刻。点图上任意位置可跳到那一帧。</p>
  {''.join(f'<div class="legend">{legend(p)}</div>{p.svg(bands)}' for p in panels)}
</div>
</div>
<script>
const D = {json.dumps(payload, ensure_ascii=False)};
const v = document.getElementById('v');
const fps = D.fps, nAvi = D.nAvi;
const R = D.rowOfAvi, T = D.t, S = D.st;
const $ = id => document.getElementById(id);
const nf = (x, d=1) => (x === null || x === undefined) ? '—' : Number(x).toFixed(d);

function clamp(x, a, b) {{ return Math.max(a, Math.min(b, x)); }}

// 当前视频帧号：容器恒定帧率，第 i 帧覆盖 [i/fps,(i+1)/fps)
function curAvi() {{
  let i = Math.floor(v.currentTime * fps + 1e-6);
  return clamp(i, 0, nAvi - 1);
}}
function rowOf(i) {{
  const r = R[i];
  return (r === null || r === undefined) ? null : r;
}}

function paint(i) {{
  const r = rowOf(i);
  $('k_avi').textContent = i + ' / ' + (nAvi - 1);
  $('pos').textContent = v.currentTime.toFixed(3) + ' s / ' + (nAvi / fps).toFixed(2) + ' s';
  if (r === null) {{
    $('k_fid').textContent = '—（该视频帧没有对应数据行）';
    ['k_t','k_rpy','k_g','k_age','k_det','k_hit','k_vt','k_drift'].forEach(k => $(k).textContent = '—');
    $('badge').textContent = '尾帧'; $('badge').style.background = '#94a3b8';
    moveHead(null); return;
  }}
  // 视频时刻（容器按恒定帧率算）与真实采集时刻的差，就是编码丢帧累积出来的偏差
  const vt = i / fps;
  const ct = T[r] - T[R[0]];
  const drift = (vt - ct) * 1000;
  $('k_vt').textContent = vt.toFixed(3) + ' s';
  $('k_drift').textContent = (drift >= 0 ? '+' : '') + drift.toFixed(0) +
    ' ms（负=视频比真实慢，因为中间编码掉了帧，画面按恒定帧率播放）';
  $('k_fid').textContent = (D.fid0 + r);
  $('k_t').textContent = T[r].toFixed(3) + ' s';
  $('k_rpy').textContent = nf(D.roll[r]) + '° / ' + nf(D.pitch[r]) + '° / ' + nf(D.yaw[r]) + '°';
  $('k_g').textContent = nf(D.gx[r]) + ' / ' + nf(D.gy[r]) + ' / ' + nf(D.gz[r]) + ' °/s';
  $('k_age').textContent = (D.age[r] === null ? '—' : D.age[r] + ' ms');
  $('k_det').textContent = (D.det[r] === null ? '—' : D.det[r] + ' ms');
  const hit = D.hit[r] === 1;
  $('k_hit').innerHTML = hit
    ? '<span class="hit">● 命中</span>　偏航 ' + nf(D.tyaw[r]) + '° 俯仰 ' + nf(D.tpitch[r]) + '°'
    : '无';
  const stt = S[r];
  const col = stt === 'FLIGHT' ? '#2563eb' : stt === 'STOP' ? '#d97706' : '#64748b';
  $('badge').textContent = stt; $('badge').style.background = col;
  moveHead(T[r]);
}}

function moveHead(tv) {{
  document.querySelectorAll('svg.panel').forEach(svg => {{
    const h = svg.querySelector('.head');
    if (!h) return;
    if (tv === null) {{ h.setAttribute('opacity', 0); return; }}
    const W = 1160, ML = 62, MR = 18;
    const x = ML + (tv / D.xhi) * (W - ML - MR);
    h.setAttribute('x1', x); h.setAttribute('x2', x); h.setAttribute('opacity', .9);
  }});
}}

// 播放/暂停/逐帧
let raf = 0;
function tick() {{
  if (!v.paused) {{ paint(curAvi()); raf = requestAnimationFrame(tick); }}
}}
v.addEventListener('play', () => {{ $('play').textContent = '暂停'; tick(); }});
v.addEventListener('pause', () => {{ $('play').textContent = '播放'; cancelAnimationFrame(raf); paint(curAvi()); }});
v.addEventListener('seeked', () => paint(curAvi()));
v.addEventListener('loadedmetadata', () => paint(curAvi()));
$('play').onclick = () => v.paused ? v.play() : v.pause();
function step(d) {{
  v.pause();
  const i = clamp(curAvi() + d, 0, nAvi - 1);
  v.currentTime = (i + 0.5) / fps;   // 落在这一帧中间，避免边界抖动
}}
$('prev').onclick = () => step(-1);
$('next').onclick = () => step(1);
document.addEventListener('keydown', e => {{
  if (e.key === 'ArrowLeft') {{ e.preventDefault(); step(-1); }}
  if (e.key === 'ArrowRight') {{ e.preventDefault(); step(1); }}
  if (e.key === ' ') {{ e.preventDefault(); $('play').click(); }}
}});
document.querySelectorAll('#spd button').forEach(b => {{
  b.onclick = () => {{
    document.querySelectorAll('#spd button').forEach(x => x.classList.remove('on'));
    b.classList.add('on'); v.playbackRate = +b.dataset.r;
  }};
}});

// 点图跳转：按真实采集时刻找最近的视频帧
document.querySelectorAll('svg.panel').forEach(svg => {{
  svg.querySelector('.clicker').addEventListener('click', ev => {{
    const r = svg.getBoundingClientRect();
    const x = (ev.clientX - r.left) / r.width * 1160;
    const W = 1160, ML = 62, MR = 18;
    const tv = clamp((x - ML) / (W - ML - MR), 0, 1) * D.xhi;
    // 在 T 里找最近的行，再由 row 反查该行对应的 avi 帧
    let lo = 0, hi = T.length - 1;
    while (hi - lo > 1) {{ const m = (lo + hi) >> 1; if (T[m] < tv) lo = m; else hi = m; }}
    const row = (Math.abs(T[lo] - tv) < Math.abs(T[hi] - tv)) ? lo : hi;
    let avi = 0;
    for (let i = 0; i < R.length; i++) if (R[i] === row) {{ avi = i; break; }}
    v.pause(); v.currentTime = (avi + 0.5) / fps;
  }});
}});

paint(0);
</script>
</body></html>
"""
    out = web / "index.html"
    out.write_text(html, encoding="utf-8")

    print(f"=== 产物 ===")
    print(f"    {out}  ({out.stat().st_size / 1024:.0f} KB)")
    print(f"    {mp4}  ({mp4.stat().st_size / 1e6:.1f} MB)")
    print(f"=== 时间线 ===")
    print(f"    起飞 {f0(t_fl, 2)}s  落地 {f0(t_sp, 2)}s  "
          f"飞行 {f0((t_sp - t_fl) if t_fl is not None and t_sp is not None else None, 2)}s")
    print(f"    直接双击 {out.name} 打开（同目录的 video.mp4 必须一起留着）")
    return out


def main():
    ap = argparse.ArgumentParser(description="视频与数据逐帧同步的网页")
    ap.add_argument("session", nargs="?", default="record")
    ap.add_argument("--crf", type=int, default=18)
    a = ap.parse_args()
    s = Path(a.session)
    if not (s / "frames.csv").exists():
        subs = sorted([p for p in s.iterdir() if p.is_dir() and (p / "frames.csv").exists()],
                      key=lambda p: (p / "frames.csv").stat().st_mtime, reverse=True)
        if not subs:
            sys.exit(f"{s} 下没有会话")
        s = subs[0]
        print(f"取最新会话: {s.name}")
    build(s, a.crf)


if __name__ == "__main__":
    main()
