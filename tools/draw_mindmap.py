#!/usr/bin/env python3
"""画一张「板端软件现状」思维导图（Pillow，纯本地、不联网、可复现）。

为什么用 Pillow 而不是 markmap/graphviz：
  - 这台机器上没有 graphviz；markmap 要 CDN，离线打不开；
  - Pillow 是 Office 依赖里自带的，画出来的 PNG 能直接贴进文档/微信。

用法：
    python tools/draw_mindmap.py                      # 输出到 docs/mindmap_status.png
    python tools/draw_mindmap.py --out D:/tmp/x.png

改内容只改下面的 MIND 结构；布局/配色/连线的逻辑不用动。
"""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# 内容：根 → 分支 → 叶子（这就是这张图的"唯一数据源"）
# ---------------------------------------------------------------------------
ROOT = "LicheeRV 镖体视觉 · 现状"

MIND = [
    ("① 采集 / 检测", (0x2F, 0x6F, 0xED), [
        "1280×720 NV12 @ 79.5–80 fps",
        "绿灯检测 5.3 ms/帧（ns=1，4×4 抽点）",
        "多目标筛选 + 卡尔曼 → yaw/pitch/vyaw/vpitch/dist",
        "像素→角度：内参仍是估算值，未标定",
    ]),
    ("② 电控通信（v2 二进制）", (0x12, 0x9E, 0x6A), [
        "上行 28 B @50 Hz：detected/角度/角速度/dist",
        "下行 33 B @100 Hz：STATE/FLAGS/欧拉角/陀螺",
        "视觉侧可只收不发（uart_tx_enable: false）",
        "未验证：与电控解析器对帧、ttyS1 排针",
    ]),
    ("③ 飞行数据记录（T+ 预录）", (0xB8, 0x6A, 0x00), [
        "预录 3 s 环形缓冲 → FLIGHT 边沿整体落盘",
        "frames.csv：每帧一行 / 26 列（含原始像素）",
        "mcu.csv：电控每帧一行 / 10 列",
        "meta.txt + health.csv + video_frames.csv",
        "合并分析：flight_merge.py → merged.csv / xlsx",
    ]),
    ("④ 运行状态 / 退出归因", (0x8B, 0x3F, 0xD6), [
        "state/run_history.log：start/ready/alive/exit/crash",
        "崩溃处理器：sig + 帧数 + 温度（异步信号安全）",
        "9 个 print_* 开关，默认全开",
        "status.sh：一条命令看进程/温度/历史/最近记录",
        "孤儿保护：监督进程没了，相机跟着退",
    ]),
    ("⑤ 实测与验证", (0xC0, 0x39, 0x2B), [
        "主机 6 套测试：162 项 0 失败",
        "记录全开 78.4–79.0 fps（基线 79.7，无回归）",
        "A/B 同温对比：新旧二进制帧率一致",
        "崩溃归因板上实测：kill -SEGV → tag=crash",
    ]),
    ("⑥ 还没做完（阻塞项）", (0x44, 0x46, 0x4A), [
        "电控 STATE 的判定语义（起飞延迟/误触发）",
        "/dev/ttyS1 是否引到排针（看原理图）",
        "相机内参标定（角度/距离才有物理意义）",
        "真实目标检出率、长跑散热（74.5 ℃ 会停）",
    ]),
]

# ---------------------------------------------------------------------------
# 样式
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",     # 微软雅黑
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\simsun.ttc",
]

BG = (0xFA, 0xFB, 0xFD)
FG = (0x22, 0x26, 0x2B)
GRID = (0xE4, 0xE8, 0xEE)

ROOT_W, ROOT_H = 430, 96
BRANCH_W, BRANCH_H = 330, 62
LEAF_PAD_X, LEAF_PAD_Y = 16, 12
LINE_H = 30

FONT_ROOT = 40
FONT_BRANCH = 27
FONT_LEAF = 23
FONT_FOOT = 18

MARGIN = 48
COL_ROOT_X = MARGIN
BRANCH_GAP = 34


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return ImageFont.load_default()


def wrap(text: str, font, max_w: int):
    """按像素宽度折行（中文没有空格，按字符累加即可）。"""
    lines, cur = [], ""
    for ch in text:
        probe = cur + ch
        w = font.getbbox(probe)[2] - font.getbbox(probe)[0]
        if w > max_w and cur:
            lines.append(cur)
            cur = ch
        else:
            cur = probe
    if cur:
        lines.append(cur)
    return lines


def bezier(p0, p1, p2, p3, steps=40):
    pts = []
    for i in range(steps + 1):
        t = i / steps
        mt = 1 - t
        x = (mt ** 3) * p0[0] + 3 * (mt ** 2) * t * p1[0] + 3 * mt * (t ** 2) * p2[0] + (t ** 3) * p3[0]
        y = (mt ** 3) * p0[1] + 3 * (mt ** 2) * t * p1[1] + 3 * mt * (t ** 2) * p2[1] + (t ** 3) * p3[1]
        pts.append((x, y))
    return pts


def shade(color, factor: float):
    return tuple(max(0, min(255, int(c + (255 - c) * factor))) for c in color)


def main() -> int:
    ap = argparse.ArgumentParser(description="画板端软件现状思维导图")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "docs", "mindmap_status.png"))
    args = ap.parse_args()

    f_root = load_font(FONT_ROOT)
    f_branch = load_font(FONT_BRANCH)
    f_leaf = load_font(FONT_LEAF)
    f_foot = load_font(FONT_FOOT)

    # ---- 宽度按文字实测：中文标题长度差别很大，写死宽度一定会截断 ----
    def text_w(font, s):
        b = font.getbbox(s)
        return b[2] - b[0]

    root_w = max(ROOT_W, text_w(f_root, ROOT) + 72)
    branch_w = max(BRANCH_W, max(text_w(f_branch, t) for t, _, _ in MIND) + 48)
    col_branch_x = COL_ROOT_X + root_w + 110
    col_leaf_x = col_branch_x + branch_w + 90
    leaf_max_w = 720

    # ---- 先算布局：每个叶子的高度、每个分支占多高 ----
    layout = []          # [(branch_title, color, [(lines, w, h)], y_center)]
    cursor_y = MARGIN + 40
    for title, color, leaves in MIND:
        items = []
        for text in leaves:
            lines = wrap(text, f_leaf, leaf_max_w)
            w = max(f_leaf.getbbox(ln)[2] - f_leaf.getbbox(ln)[0] for ln in lines) + LEAF_PAD_X * 2
            h = len(lines) * LINE_H + LEAF_PAD_Y * 2
            items.append((lines, w, h))
        block_h = sum(it[2] for it in items) + 14 * (len(items) - 1)
        layout.append([title, color, items, cursor_y + block_h / 2, block_h])
        cursor_y += block_h + BRANCH_GAP

    canvas_h = int(cursor_y + MARGIN + 60)
    canvas_w = col_leaf_x + leaf_max_w + MARGIN + 40
    img = Image.new("RGB", (canvas_w, canvas_h), BG)
    dr = ImageDraw.Draw(img)

    # 背景网格（很淡，只是为了看图不飘）
    for gy in range(0, canvas_h, 40):
        dr.line([(0, gy), (canvas_w, gy)], fill=GRID)
    for gx in range(0, canvas_w, 40):
        dr.line([(gx, 0), (gx, canvas_h)], fill=GRID)

    root_y = (layout[0][3] + layout[-1][3]) / 2

    # ---- 连线：根 → 分支（粗曲线），分支 → 叶子（细曲线）----
    root_anchor = (COL_ROOT_X + root_w, root_y)
    for title, color, items, by, block_h in layout:
        branch_anchor = (col_branch_x, by)
        dr.line(bezier(root_anchor,
                       (root_anchor[0] + 70, root_anchor[1]),
                       (branch_anchor[0] - 70, branch_anchor[1]),
                       branch_anchor), fill=color, width=4, joint="curve")
        y = by - block_h / 2
        for lines, w, h in items:
            leaf_anchor = (col_leaf_x, y + h / 2)
            dr.line(bezier((col_branch_x + branch_w, by),
                           (col_branch_x + branch_w + 40, by),
                           (leaf_anchor[0] - 40, leaf_anchor[1]),
                           leaf_anchor), fill=shade(color, 0.45), width=2, joint="curve")
            y += h + 14

    # ---- 根节点 ----
    dr.rounded_rectangle([COL_ROOT_X, root_y - ROOT_H / 2, COL_ROOT_X + root_w, root_y + ROOT_H / 2],
                         radius=22, fill=(0x1F, 0x24, 0x2B), outline=None)
    tw = f_root.getbbox(ROOT)[2] - f_root.getbbox(ROOT)[0]
    dr.text((COL_ROOT_X + (root_w - tw) / 2, root_y - FONT_ROOT * 0.62), ROOT, font=f_root,
            fill=(0xFF, 0xFF, 0xFF))

    # ---- 分支与叶子 ----
    for title, color, items, by, block_h in layout:
        dr.rounded_rectangle([col_branch_x, by - BRANCH_H / 2,
                              col_branch_x + branch_w, by + BRANCH_H / 2],
                             radius=16, fill=color)
        tw = f_branch.getbbox(title)[2] - f_branch.getbbox(title)[0]
        dr.text((col_branch_x + 22, by - FONT_BRANCH * 0.66), title, font=f_branch,
                fill=(0xFF, 0xFF, 0xFF))
        y = by - block_h / 2
        for lines, w, h in items:
            box = [col_leaf_x, y, col_leaf_x + w, y + h]
            dr.rounded_rectangle(box, radius=12, fill=(0xFF, 0xFF, 0xFF),
                                 outline=shade(color, 0.55), width=2)
            ty = y + LEAF_PAD_Y
            for ln in lines:
                dr.text((col_leaf_x + LEAF_PAD_X, ty), ln, font=f_leaf, fill=FG)
                ty += LINE_H
            y += h + 14

    # ---- 页脚 ----
    foot = "生成：tools/draw_mindmap.py（Pillow 本地绘制） · 数据源：本文件 MIND 结构"
    dr.text((MARGIN, canvas_h - MARGIN + 6), foot, font=f_foot, fill=(0x88, 0x8E, 0x96))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    img.save(args.out)
    print("wrote %s  (%dx%d)" % (args.out, img.width, img.height))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
