#!/usr/bin/env python3
"""生成 OSD 用的 8×8 位图字库（ASCII 32..95），输出 src/osd_font.h。

为什么这么做：
  · 板端没有 freetype / OpenCV，画文字只能自己点像素；
  · 手写字模容易错，直接用 Windows 自带字体渲染 + 阈值化，结果可肉眼核对（tests/osd_test 会出样张）；
  · 只覆盖 ASCII（数字、大写字母、常用符号）——OSD 上要显示的是 FLIGHT/GX/-5.25 这类，不需要中文。

用法：python tools/gen_osd_font.py
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FIRST, LAST = 32, 95      # 空格 .. 下划线
CELL = 8                  # 字符单元宽 8 像素
FONT = "C:/Windows/Fonts/msyhbd.ttc"
BIG = 32                  # 先在 32px 上渲染，再等比缩小
CELL_H = 16               # 字符单元高 16 像素（竖分辨率翻倍，8×8 放大后太"马赛克"）
CAP_TARGET = 11           # 大写字母墨迹高度目标像素（16px 里留 2px 顶空 + 3px 基线）
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "src", "osd_font.h")

_font = None
_scale = None


def _init():
    """算一次缩放比：让 'H' 的墨迹高度正好 CAP_TARGET 像素。

    不能"每个字符各自缩放填满单元"——那样句点会被放大成一坨方块。
    统一缩放比才能保持字符之间的相对大小。
    """
    global _font, _scale
    if _font is not None:
        return
    _font = ImageFont.truetype(FONT, BIG)
    im = Image.new("L", (BIG * 2, BIG * 2), 0)
    d = ImageDraw.Draw(im)
    b = d.textbbox((0, 0), "H", font=_font)
    d.text((4 - b[0], 4 - b[1]), "H", font=_font, fill=255)
    bb = im.getbbox()
    cap_h = (bb[3] - bb[1]) if bb else BIG
    _scale = CAP_TARGET / cap_h


def render(ch):
    _init()
    img = Image.new("L", (BIG * 2, BIG * 2), 0)
    d = ImageDraw.Draw(img)
    b = d.textbbox((0, 0), ch, font=_font)
    d.text((4 - b[0], 4 - b[1]), ch, font=_font, fill=255)
    bb = img.getbbox()
    if bb is None:                    # 空格
        return [0] * CELL_H
    ink = img.crop(bb)
    # 只"缩"不"放"：M/W 这类宽字按单元宽收一下，句点之类的小字保持原尺寸
    s = _scale
    if ink.width * s > CELL:
        s = CELL / ink.width
    w = max(1, int(round(ink.width * s)))
    h = max(1, int(round(ink.height * s)))
    ink = ink.resize((w, h), Image.LANCZOS)
    cell = Image.new("L", (CELL, CELL_H), 0)
    # 底对齐（所有字坐在同一条基线上）：句点/减号才不会跑到中间像横线
    cell.paste(ink, ((CELL - w) // 2, max(0, CELL_H - 3 - h)))
    rows = []
    for y in range(CELL_H):
        bits = 0
        for x in range(CELL):
            if cell.getpixel((x, y)) >= 110:
                bits |= 1 << (7 - x)   # 最高位 = 最左像素
        rows.append(bits)
    if ch == "-":                     # 连字符在缩放后会细到看不见，直接画一根 4px 的横杠
        rows = [0] * CELL_H
        rows[CELL_H // 2] = 0b00111100
    return rows


def main():
    lines = [
        "// 自动生成，不要手改：python tools/gen_osd_font.py",
        "// 8×16 位图字库，覆盖 ASCII 32..95；每个字符 16 字节，每字节一行（高位 = 最左像素）。",
        "#ifndef GREEN_TARGET_OSD_FONT_H_",
        "#define GREEN_TARGET_OSD_FONT_H_",
        "",
        "#include <stdint.h>",
        "",
        "namespace gt { namespace osd {",
        "",
        "static const int kFontFirst = %d;   // 字库第一个字符" % FIRST,
        "static const int kFontLast = %d;    // 最后一个字符" % LAST,
        "static const int kFontW = %d;" % CELL,
        "static const int kFontH = %d;" % CELL_H,
        "",
        "static const uint8_t kFont8x8[] = {",
    ]
    for code in range(FIRST, LAST + 1):
        rows = render(chr(code))
        label = "space" if code == 32 else chr(code)
        lines.append("  %s,  /* '%s' */" % (", ".join("0x%02X" % r for r in rows), label))
    lines += [
        "};",
        "",
        "}}  // namespace gt::osd",
        "",
        "#endif  // GREEN_TARGET_OSD_FONT_H_",
        "",
    ]
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote", OUT, "chars=", LAST - FIRST + 1)


if __name__ == "__main__":
    sys.exit(main())
