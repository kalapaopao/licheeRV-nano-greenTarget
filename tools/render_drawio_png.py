# -*- coding: utf-8 -*-
"""把 drawio（含配色）渲染成高清 PNG：直线连线 + 自适应字号 + 圆角框。"""
import re
import html
import math
import sys
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw, ImageFont

SRC = sys.argv[1]
OUT = sys.argv[2]
SCALE = 2.0
FONT = "C:/Windows/Fonts/msyh.ttc"


def clean(v):
    if not v:
        return ""
    v = re.sub(r"<br\s*/?>", "\n", v, flags=re.I)
    v = re.sub(r"<[^>]+>", "", v)
    return html.unescape(v).replace("\xa0", " ").strip()


def smap(s):
    d = {}
    for p in (s or "").split(";"):
        if "=" in p:
            k, v = p.split("=", 1)
            d[k.strip()] = v.strip()
    return d


def color(v, default):
    return v if v and v.startswith("#") else default


root = ET.parse(SRC).getroot()
model = root.find("diagram").find("mxGraphModel")
raw = []
for c in model.find("root").findall("mxCell"):
    g = c.find("mxGeometry")
    if g is None:
        continue
    if c.get("vertex") == "1":
        w = float(g.get("width", 0) or 0)
        h = float(g.get("height", 0) or 0)
        if w > 2 and h > 2:
            raw.append(dict(kind="v", id=c.get("id"), x=float(g.get("x", 0) or 0),
                            y=float(g.get("y", 0) or 0), w=w, h=h,
                            text=clean(c.get("value")), st=smap(c.get("style"))))
    elif c.get("edge") == "1":
        raw.append(dict(kind="e", id=c.get("id"), src=c.get("source"),
                        dst=c.get("target"), st=smap(c.get("style"))))

verts = [r for r in raw if r["kind"] == "v"]
edges = [r for r in raw if r["kind"] == "e"]
for v in verts:
    v["band"] = (v["text"] == "" and (v["w"] > 200 or v["h"] > 150))

minx = min(v["x"] for v in verts)
miny = min(v["y"] for v in verts)
maxx = max(v["x"] + v["w"] for v in verts)
maxy = max(v["y"] + v["h"] for v in verts)
pad = 26
W = int((maxx - minx + 2 * pad) * SCALE)
H = int((maxy - miny + 2 * pad) * SCALE)
img = Image.new("RGB", (W, H), "#FFFFFF")
dr = ImageDraw.Draw(img)

pos = {v["id"]: ((v["x"] - minx + pad) * SCALE, (v["y"] - miny + pad) * SCALE,
                 v["w"] * SCALE, v["h"] * SCALE) for v in verts}

# 区域底色
for v in verts:
    if not v["band"]:
        continue
    x0, y0, w, h = pos[v["id"]]
    dr.rectangle([x0, y0, x0 + w, y0 + h],
                 fill=color(v["st"].get("fillColor"), "#FFFFFF"),
                 outline=color(v["st"].get("strokeColor"), "#BFBFBF"),
                 width=max(2, int(1.2 * SCALE)))

# 连线
for e in edges:
    if e["src"] not in pos or e["dst"] not in pos:
        continue
    s, t = pos[e["src"]], pos[e["dst"]]
    scx, scy = s[0] + s[2] / 2, s[1] + s[3] / 2
    tcx, tcy = t[0] + t[2] / 2, t[1] + t[3] / 2
    if t[0] > s[0] + s[2] + 4:            # 目标在右
        p0 = (s[0] + s[2], scy); p1 = (t[0], tcy)
        if abs(p1[1] - p0[1]) < 6:
            pts = [p0, p1]
        else:
            mx = (p0[0] + p1[0]) / 2
            pts = [p0, (mx, p0[1]), (mx, p1[1]), p1]
    elif t[0] + t[2] < s[0] - 4:          # 目标在左
        p0 = (s[0], scy); p1 = (t[0] + t[2], tcy)
        if abs(p1[1] - p0[1]) < 6:
            pts = [p0, p1]
        else:
            mx = (p0[0] + p1[0]) / 2
            pts = [p0, (mx, p0[1]), (mx, p1[1]), p1]
    elif tcy > scy:                        # 目标在下
        p0 = (scx, s[1] + s[3]); p1 = (tcx, t[1])
        if abs(p1[0] - p0[0]) < 6:
            pts = [p0, p1]
        else:
            my = (p0[1] + p1[1]) / 2
            pts = [p0, (p0[0], my), (p1[0], my), p1]
    else:                                  # 目标在上
        p0 = (scx, s[1]); p1 = (tcx, t[1] + t[3])
        if abs(p1[0] - p0[0]) < 6:
            pts = [p0, p1]
        else:
            my = (p0[1] + p1[1]) / 2
            pts = [p0, (p0[0], my), (p1[0], my), p1]
    col = color(e["st"].get("strokeColor"), "#2F5597")
    wd = max(2, int(round(float(e["st"].get("strokeWidth", "1") or 1) * SCALE)))
    dr.line(pts, fill=col, width=wd, joint="curve")
    p0, p1 = pts[-2], pts[-1]
    ang = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
    L = 9 * SCALE
    for d in (0.38, -0.38):
        dr.line([p1, (p1[0] - L * math.cos(ang + d), p1[1] - L * math.sin(ang + d))], fill=col, width=wd)

# 框 + 文字
for v in verts:
    if v["band"]:
        continue
    x0, y0, w, h = pos[v["id"]]
    fill = color(v["st"].get("fillColor"), "#FFFFFF")
    stroke = color(v["st"].get("strokeColor"), "#666666")
    fc = color(v["st"].get("fontColor"), "#1A1A1A")
    rad = 8 * SCALE if v["st"].get("rounded") == "1" else 0
    dr.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=rad, fill=fill, outline=stroke,
                         width=max(2, int(1.2 * SCALE)))
    if not v["text"]:
        continue
    maxw = w - 8 * SCALE
    chosen = None
    for fs in (12, 11, 10, 9, 8):
        f = ImageFont.truetype(FONT, int(fs * SCALE))
        lines = []
        for para in v["text"].split("\n"):
            cur = ""
            for ch in para:
                if dr.textlength(cur + ch, font=f) <= maxw:
                    cur += ch
                else:
                    lines.append(cur)
                    cur = ch
            lines.append(cur)
        lh = fs * SCALE * 1.35
        if len(lines) * lh <= h - 6 * SCALE and all(dr.textlength(l, font=f) <= maxw for l in lines):
            chosen = (f, lines, lh)
            break
    if chosen is None:
        f = ImageFont.truetype(FONT, int(8 * SCALE))
        lines = [v["text"]]
        chosen = (f, lines, 8 * SCALE * 1.35)
    f, lines, lh = chosen
    cy = y0 + (h - len(lines) * lh) / 2
    for ln in lines:
        tw = dr.textlength(ln, font=f)
        dr.text((x0 + (w - tw) / 2, cy), ln, font=f, fill=fc)
        cy += lh

img.save(OUT)
print("saved", OUT, img.size)
