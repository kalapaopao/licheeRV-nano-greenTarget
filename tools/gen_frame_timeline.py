#!/usr/bin/env python3
"""生成「两个数据流的时间轴」示意图：直观看出 100Hz 电控帧与 80Hz 相机帧怎么错位、
为什么每帧记的是「左边最近那一帧」而不是「最近那一帧」。

为什么用 Pillow 而不是 drawio：时间轴要按毫秒精确定位刻度、画括号和虚线，
用绘图库算坐标比在 drawio 里摆几十个小方块可靠。

用法：python tools/gen_frame_timeline.py docs/architecture/frame_timeline.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "C:/Windows/Fonts/msyh.ttc"
W, H = 1500, 900
S = 2                      # 输出放大倍数
BG = (0xFA, 0xFB, 0xFD)
INK = (0x22, 0x26, 0x2B)
GREY = (0x88, 0x8E, 0x96)
PURPLE = (0x70, 0x30, 0xA0)
BLUE = (0x2F, 0x55, 0x97)
ORANGE = (0xC5, 0x5A, 0x11)
GREEN = (0x2E, 0x7D, 0x6B)

X0, X1 = 150, 1400        # 时间轴左右边界（像素）
T_MAX = 46.0              # 时间轴跨度（毫秒）
Y_MCU, Y_CAM = 170, 300   # 两条泳道的基线
Y_BAND = 58               # 泳道半高
Y_AXIS = 560              # 时间轴线

# 100Hz 电控帧（每 10ms 一帧）
MCU = [(0, 90), (10, 91), (20, 92), (30, 93), (40, 94)]
# 80Hz 相机帧（每 12.5ms 一帧）
CAM = [(3.0, 698), (15.5, 699), (28.0, 700), (40.5, 701)]


def load(size):
    for p in (FONT, "C:/Windows/Fonts/simhei.ttf"):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def tx(ms):
    return X0 + (X1 - X0) * ms / T_MAX


def main() -> int:
    out = sys.argv[1]
    img = Image.new("RGB", (W, H), BG)      # 1× 画，最后整体放大当超采样
    d = ImageDraw.Draw(img)
    f_title = load(26)
    f_lane = load(17)
    f_tick = load(14)
    f_note = load(14)
    f_small = load(13)

    def T(x, y, s, f, fill=INK, anchor="la"):
        d.text((x, y), s, font=f, fill=fill, anchor=anchor)

    # ---------- 标题 ----------
    T(X0 - 30, 26, "两个数据流怎么错位：100Hz 电控帧 vs 80Hz 相机帧", f_title)
    T(X0 - 30, 64, "时间单位：毫秒（板端真实记录用微秒；这里按理想周期画，只为看清关系）",
      f_small, GREY)

    # ---------- 两条泳道 ----------
    d.rounded_rectangle([X0 - 10, Y_MCU - Y_BAND, X1 + 10, Y_MCU + Y_BAND], radius=8,
                        fill=(0xF4, 0xF0, 0xFA), outline=(0xD8, 0xCC, 0xEA), width=2)
    d.rounded_rectangle([X0 - 10, Y_CAM - Y_BAND, X1 + 10, Y_CAM + Y_BAND], radius=8,
                        fill=(0xEE, 0xF4, 0xFD), outline=(0xC9, 0xD8, 0xEF), width=2)
    T(X0, Y_MCU - Y_BAND + 10, "电控 → 视觉（0x02，100Hz，每 10ms 一帧；我们左右不了它的节奏）",
      f_lane, PURPLE)
    T(X0, Y_CAM - Y_BAND + 10, "相机帧（80Hz，每 12.5ms 一帧；timeout 会漏帧，frame_id 会跳号）",
      f_lane, BLUE)

    # ---------- 时间轴 ----------
    d.line([(X0, Y_AXIS), (X1, Y_AXIS)], fill=GREY, width=3)
    for ms in range(0, int(T_MAX) + 1, 5):
        x = tx(ms)
        d.line([(x, Y_AXIS - 6), (x, Y_AXIS + 6)], fill=GREY, width=2)
        if ms % 10 == 0:
            T(x, Y_AXIS + 12, "%d" % ms, f_tick, GREY, anchor="ma")
    T(X1 + 12, Y_AXIS - 10, "ms", f_tick, GREY)

    # ---------- 电控帧刻度 ----------
    for ms, seq in MCU:
        x = tx(ms)
        d.line([(x, Y_MCU - 30), (x, Y_MCU + 26)], fill=PURPLE, width=4)
        d.ellipse([x - 5, Y_MCU - 5, x + 5, Y_MCU + 5], fill=PURPLE)
        T(x, Y_MCU + 32, "#%d" % seq, f_tick, PURPLE, anchor="ma")

    # ---------- 相机帧刻度 ----------
    for ms, fid in CAM:
        x = tx(ms)
        d.polygon([(x, Y_CAM - 26), (x - 16, Y_CAM + 18), (x + 16, Y_CAM + 18)],
                  fill=BLUE, outline=BLUE)
        T(x, Y_CAM + 24, "F%d" % fid, f_tick, BLUE, anchor="ma")

    # ---------- 例 1：F699（15.5ms）落在 #91(10) 与 #92(20) 之间 ----------
    x_cam1, x_p1, x_f1 = tx(15.5), tx(10), tx(20)
    d.line([(x_cam1, Y_CAM + Y_BAND), (x_cam1, 470)], fill=BLUE, width=2)
    d.line([(x_p1, Y_MCU + Y_BAND), (x_p1, 430)], fill=PURPLE, width=2)
    d.line([(x_f1, Y_MCU + Y_BAND), (x_f1, 470)], fill=PURPLE, width=2)
    d.line([(x_cam1, 390), (x_f1, 390)], fill=ORANGE, width=5)     # 未来那帧
    d.line([(x_p1, 430), (x_cam1, 430)], fill=GREEN, width=5)      # 过去那帧
    T((x_cam1 + x_f1) / 2, 380, "4.5ms（#92 在未来，不能用）", f_note, ORANGE, anchor="mb")
    T((x_p1 + x_cam1) / 2, 420, "往回 5.5ms → 取 #91", f_note, GREEN, anchor="mb")
    T(x_cam1 + 12, 404, "F699 取帧这一刻", f_small, BLUE)

    # ---------- 例 2：F700（28ms）→ 取 #92 ----------
    x_cam2, x_p2 = tx(28.0), tx(20)
    d.line([(x_cam2, Y_CAM + Y_BAND), (x_cam2, 500)], fill=BLUE, width=2)
    d.line([(x_p2, Y_MCU + Y_BAND), (x_p2, 500)], fill=PURPLE, width=2)
    d.line([(x_p2, 500), (x_cam2, 500)], fill=GREEN, width=5)
    T((x_p2 + x_cam2) / 2, 490, "往回 8.0ms → 取 #92，写进 mcu_age_ms", f_note, GREEN,
      anchor="mb")

    # ---------- 结论 ----------
    d.rounded_rectangle([X0 - 30, 600, X1 + 20, 830], radius=10, fill=(255, 255, 255),
                        outline=(0xD0, 0xD5, 0xDC), width=2)
    T(X0 - 12, 616, "为什么取「左边最近」而不是「最近」　　绿段 = 实际采用　　橙段 = 更近，但在未来，不能用", f_lane, INK)
    lines = [
        "· 取帧那一刻，电控下一帧（#92，20ms）还没到；用它就是「用了未来数据」，控制里叫 look-ahead，不能做。",
        "· 规则：取「采集时刻之前、最近的那一帧」——就是 flight_merge.py 里 direction=backward 的含义。",
        "· 差多少如实记进 mcu_age_ms（F699=5.5ms、F700=8.0ms）；事后可以按这个值筛掉太旧的帧。",
        "· 一行里两个时刻：capture_us（F700 取帧）+ mcu_rx_us（#92 到达）→ 差值 = age。",
        "· 电控 100Hz、相机 80Hz，永远不同步；插值会「编」出不存在的采样，所以不插值。",
    ]
    y = 646
    for s in lines:
        T(X0 - 12, y, s, f_note, GREEN if s.startswith("· 一行") else INK)
        y += 28

    img = img.resize((W * S, H * S), Image.LANCZOS)
    img.save(out)
    print("saved", out, img.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
