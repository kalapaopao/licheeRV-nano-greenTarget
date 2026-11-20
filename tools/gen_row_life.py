#!/usr/bin/env python3
"""生成「一行数据的一生」流程图：从电控帧到达 → 快照 → 取帧 → 组行 → 入队 → 写盘。

回答三个常见疑问：
  1. 一行里到底有几个时刻？——两个：capture_us（画面）和 mcu_rx_us（电控数据）。
  2. 写盘晚了几毫秒，会不会把时刻弄乱？——不会，写线程只搬运文本。
  3. 为什么要“取帧那一刻立刻读快照”？——晚读会拿到更新的电控帧（实测新 0~9ms）。

用法：python tools/gen_row_life.py docs/architecture/row_life.drawio
"""
import sys
import xml.etree.ElementTree as ET

ELEC = "whiteSpace=wrap;html=1;fillColor=#E8E1F2;strokeColor=#7030A0;rounded=1;arcSize=8;"
CAM = "whiteSpace=wrap;html=1;fillColor=#DEEBF7;strokeColor=#2F5597;rounded=1;arcSize=8;"
BUF = "whiteSpace=wrap;html=1;fillColor=#FFF2CC;strokeColor=#BF9000;rounded=1;arcSize=8;"
LOG = "whiteSpace=wrap;html=1;fillColor=#DAEEF3;strokeColor=#1F6F8B;rounded=1;arcSize=8;"
NOTE = "whiteSpace=wrap;html=1;fillColor=#FFFFFF;strokeColor=#A6A6A6;rounded=0;arcSize=8;"
WARN = "whiteSpace=wrap;html=1;fillColor=#FDE9E0;strokeColor=#C55A11;rounded=1;arcSize=8;"
EDGE = ("edgeStyle=orthogonalEdgeStyle;rounded=0;orthogonalLoop=1;jettySize=auto;"
        "html=1;defaultLabelBackgroundColor=page;")

LEFT_X, LEFT_W = 150, 560
RIGHT_X, RIGHT_W = 770, 380

# (id, 文本, 相对 y, 样式)
MAIN = [
    ("r1", "① 电控第 93 帧到达板子<br><b>rx_us = 852982879 µs</b>（这是电控数据的时刻）", 40, ELEC),
    ("r2", "② 串口线程解析 → 加锁写共享快照<br>{state, flags, roll…gz, rx_us}", 150, ELEC),
    ("r3", "③ 主线程取到一帧（VI 出图）<br><b>capture_us = 852990741 µs</b>（这是画面的时刻）", 270, CAM),
    ("r4", "④ <b>立刻</b>加锁读快照 → 拿到第 93 帧<br>记下 mcu_rx_us=852982879、age=<b>7862 µs</b>", 380, CAM),
    ("r5", "⑤ 检测 7.1 ms → 组一行文本<br>22 个视觉列 + 9 个电控列 + 2 个时刻列", 500, CAM),
    ("r6", "⑥ memcpy 进环形缓冲（微秒级，不碰磁盘）<br>队列里放的是「已经拼好的一行字符串」", 610, BUF),
    ("r7", "⑦ 写线程 write() → frames.csv<br><b>只搬运，不加任何时刻</b>（晚 20 ms 也不改 age）", 720, LOG),
]

SIDE = [
    ("s2", "共享快照 = 类成员变量 + 一把 mutex<br>（不是 static，效果等价：全局唯一状态）", 155, 90),
    ("s4", "为什么必须「立刻」读：<br>等检测（5~20ms）之后再读，<br>期间新到的电控帧会被算进这一帧<br>（实测会新 0~9ms，中位 6ms）", 380, 150),
    ("s6", "环形缓冲 2048 行（≈11s）<br>写盘跟不上：待机丢最旧、会话中丢最新<br>并计数 dropped", 610, 130),
    ("s7", "所以：一行的“新旧”只看 mcu_age_ms，<br>与磁盘什么时候写完无关", 720, 120),
]


def add(root, cid, value, x, y, w, h, style):
    c = ET.SubElement(root, "mxCell", {"id": cid, "parent": "1", "style": style,
                                       "value": value, "vertex": "1"})
    ET.SubElement(c, "mxGeometry", {"height": str(h), "width": str(w),
                                    "x": str(x), "y": str(y), "as": "geometry"})


def link(root, eid, src, dst, color="#2F5597"):
    c = ET.SubElement(root, "mxCell", {"id": eid, "parent": "1",
                                       "style": EDGE + "strokeColor=%s;strokeWidth=2;" % color,
                                       "edge": "1", "source": src, "target": dst})
    ET.SubElement(c, "mxGeometry", {"relative": "1", "as": "geometry"})


def main() -> int:
    out = sys.argv[1]
    mxfile = ET.Element("mxfile", {"host": "Electron", "type": "device"})
    diagram = ET.SubElement(mxfile, "diagram", {"name": "一行数据的一生", "id": "row-life"})
    model = ET.SubElement(diagram, "mxGraphModel", {
        "dx": "800", "dy": "600", "grid": "1", "gridSize": "10", "guides": "1",
        "tooltips": "1", "connect": "1", "arrows": "1", "fold": "1", "page": "1",
        "pageScale": "1", "pageWidth": "1250", "pageHeight": "1000",
        "math": "0", "shadow": "0"})
    root = ET.SubElement(model, "root")
    ET.SubElement(root, "mxCell", {"id": "0"})
    ET.SubElement(root, "mxCell", {"id": "1", "parent": "0"})

    for cid, text, y, style in MAIN:
        add(root, cid, text, LEFT_X, y, LEFT_W, 80, style)
    for cid, text, y, h in SIDE:
        add(root, cid, text, RIGHT_X, y, RIGHT_W, h, NOTE)

    for i in range(1, 7):
        link(root, "e%d" % i, "r%d" % i, "r%d" % (i + 1),
             "#7030A0" if i <= 2 else ("#2F5597" if i <= 4 else "#BF9000"))

    # 底部总结：一行里到底有几个时刻
    add(root, "sum", "【结论】一行只有两个时刻：画面 capture_us、电控 mcu_rx_us；差值 = mcu_age_ms（这一行标着 7862 µs）。<br>"
                     "还有两个「不知道」的偏移：曝光时刻比 capture_us 早 5~15 ms（未标定）；电控采样时刻比 rx_us 早 3~13 ms（传输+周期）。<br>"
                     "要消掉后两个，只能靠：① 闪灯标定曝光延迟；② 电控在帧里带自己的时间戳（TLV 提案 TAG 0x01）。",
        150, 860, 1080, 130, WARN)

    ET.ElementTree(mxfile).write(out, encoding="utf-8", xml_declaration=False)
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
