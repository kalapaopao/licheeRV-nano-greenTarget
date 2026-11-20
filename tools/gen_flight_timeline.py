#!/usr/bin/env python3
"""生成「一次飞行的时间线」流程图（drawio + 配套 PNG 由 tools/render_drawio_png.py 渲染）。

这条线回答的是：从通电到落地，程序在什么时刻做了什么、数据在哪一步变成文件。
"""
import sys
import xml.etree.ElementTree as ET

STAGE = ("whiteSpace=wrap;html=1;fillColor=#DEEBF7;strokeColor=#2F5597;"
         "rounded=1;arcSize=8;")
DECIDE = ("whiteSpace=wrap;html=1;fillColor=#FFF2CC;strokeColor=#BF9000;"
          "rounded=1;arcSize=8;")
OUT = ("whiteSpace=wrap;html=1;fillColor=#DAEEF3;strokeColor=#1F6F8B;"
       "rounded=1;arcSize=8;")
DIAG = ("whiteSpace=wrap;html=1;fillColor=#E4DFEC;strokeColor=#60497A;"
        "rounded=1;arcSize=8;")
EDGE = ("edgeStyle=orthogonalEdgeStyle;rounded=0;orthogonalLoop=1;jettySize=auto;"
        "html=1;defaultLabelBackgroundColor=page;")

# (id, 文本, x, y, w, h, 样式)
NODES = [
    ("t1", "① 通电\nstart.sh 监督进程\n切 36MHz 时钟 · 相机锁 · 温度检查 · 校验 config", 120, 40, 440, 70, STAGE),
    ("t2", "② camera 启动\nVI/ISP/VENC 就绪\n写 run_history：tag=ready", 120, 150, 440, 70, STAGE),
    ("t3", "③ 待机预录（不写盘）\n每帧 on_frame 进环形缓冲（≈11s）\n电控 100Hz 帧进同一条缓冲", 120, 260, 440, 70, STAGE),
    ("t4", "④ 电控 STATE：STATIC → FLIGHT", 120, 370, 440, 56, DECIDE),
    ("t5", "⑤ 开一段会话\n把前 3s 预录整体落盘，之后实时写", 120, 466, 440, 70, STAGE),
    ("t6", "⑥ 电控 STATE：FLIGHT → STOP", 120, 576, 440, 56, DECIDE),
    ("t7", "⑦ 再写 2s 收尾\nmeta.txt 记 close_reason=landed", 120, 672, 440, 70, STAGE),
    ("t8", "⑧ 退出（跑满时长 / stop.sh / 崩了）\nrun_history：tag=exit reason=… 或 tag=crash sig=…", 120, 782, 440, 70, STAGE),

    # 右侧：每一步产出/可见的东西
    ("o2", "logs/health_*.csv：每秒一行", 640, 150, 300, 56, OUT),
    ("o5", "logs/sess_*/frames.csv\n一帧一行 + 该帧电控数据（预录不丢开头）", 640, 466, 300, 70, OUT),
    ("o7", "meta.txt：触发时刻 / 行数 / 丢行数", 640, 672, 300, 56, OUT),
    ("o8", "state/run_history.log 留痕\nstatus.sh 一条命令复核", 640, 782, 300, 70, DIAG),
]

EDGES = [
    ("e1", "t1", "t2"), ("e2", "t2", "t3"), ("e3", "t3", "t4"), ("e4", "t4", "t5"),
    ("e5", "t5", "t6"), ("e6", "t6", "t7"), ("e7", "t7", "t8"),
    ("eo2", "t2", "o2"), ("eo5", "t5", "o5"), ("eo7", "t7", "o7"), ("eo8", "t8", "o8"),
]


def main() -> int:
    out_path = sys.argv[1]
    mxfile = ET.Element("mxfile", {"host": "Electron", "type": "device"})
    diagram = ET.SubElement(mxfile, "diagram", {"name": "一次飞行的时间线",
                                                "id": "flight-timeline"})
    model = ET.SubElement(diagram, "mxGraphModel", {
        "dx": "800", "dy": "600", "grid": "1", "gridSize": "10", "guides": "1",
        "tooltips": "1", "connect": "1", "arrows": "1", "fold": "1", "page": "1",
        "pageScale": "1", "pageWidth": "1100", "pageHeight": "900",
        "math": "0", "shadow": "0"})
    root = ET.SubElement(model, "root")
    ET.SubElement(root, "mxCell", {"id": "0"})
    ET.SubElement(root, "mxCell", {"id": "1", "parent": "0"})

    for nid, text, x, y, w, h, style in NODES:
        c = ET.SubElement(root, "mxCell", {"id": nid, "parent": "1", "style": style,
                                           "value": text.replace("\n", "<br>"),
                                           "vertex": "1"})
        ET.SubElement(c, "mxGeometry", {"height": str(h), "width": str(w),
                                        "x": str(x), "y": str(y), "as": "geometry"})

    for eid, src, dst in EDGES:
        c = ET.SubElement(root, "mxCell", {"id": eid, "parent": "1", "style": EDGE,
                                           "edge": "1", "source": src, "target": dst})
        ET.SubElement(c, "mxGeometry", {"relative": "1", "as": "geometry"})

    ET.ElementTree(mxfile).write(out_path, encoding="utf-8", xml_declaration=False)
    print("wrote", out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
