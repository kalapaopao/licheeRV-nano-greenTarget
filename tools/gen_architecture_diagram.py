#!/usr/bin/env python3
"""在原始 drawio 上补画「线程边界 + 记录器 + 诊断」——恢复成色带版布局。

版式（不是五张等大卡片，而是按内容自然大小分区）：
    [硬件]  [① 主线程色带]   [② 编码色带]
            [④ 串口色带]     [⑤ 记录器色带] [③ HTTP / 诊断]
    这样跨分区的连线不会穿过别的卡片。

用法：python tools/gen_architecture_diagram.py <原图.drawio> <输出.drawio>
"""
import sys
import xml.etree.ElementTree as ET

CAM_BAND = "pKCW-2MlPlMVXdzMdy-7-9"
MAIN_TITLE = "pKCW-2MlPlMVXdzMdy-7-37"
ENC_TITLE = "pKCW-2MlPlMVXdzMdy-7-52"
PENDING = "pKCW-2MlPlMVXdzMdy-7-49"
UART_BOX = "uart-thread-1"
AVI_BOX = "pKCW-2MlPlMVXdzMdy-7-62"
HTTP_READ = "pKCW-2MlPlMVXdzMdy-7-105"

STYLE_BAND = {
    "uart": "whiteSpace=wrap;html=1;fontStyle=1;fillColor=#F4F0FA;strokeColor=#B4A0D0;rounded=0;arcSize=8;",
    "rec": "whiteSpace=wrap;html=1;fontStyle=1;fillColor=#F0F9FC;strokeColor=#8CC7D8;rounded=0;arcSize=8;",
    "http": "whiteSpace=wrap;html=1;fontStyle=1;fillColor=#F1F8F6;strokeColor=#8FBFB3;rounded=0;arcSize=8;",
    "diag": "whiteSpace=wrap;html=1;fontStyle=1;fillColor=#F6F3FA;strokeColor=#B4A6C9;rounded=0;arcSize=8;",
}
BOX = {
    "purple": "whiteSpace=wrap;html=1;fillColor=#E8E1F2;strokeColor=#7030A0;rounded=1;arcSize=8;",
    "teal": "whiteSpace=wrap;html=1;fillColor=#EAF6FA;strokeColor=#1F6F8B;rounded=1;arcSize=8;",
    "teal2": "whiteSpace=wrap;html=1;fillColor=#D3ECF3;strokeColor=#1F6F8B;rounded=1;arcSize=8;",
    "green": "whiteSpace=wrap;html=1;fillColor=#E6F4EE;strokeColor=#2E7D6B;rounded=1;arcSize=8;",
    "diag": "whiteSpace=wrap;html=1;fillColor=#EFEAF5;strokeColor=#60497A;rounded=1;arcSize=8;",
    "mcu": "whiteSpace=wrap;html=1;fillColor=#FDE9E0;strokeColor=#C55A11;rounded=1;arcSize=8;",
    "note": "whiteSpace=wrap;html=1;fillColor=#FFFFFF;strokeColor=#A6A6A6;rounded=0;arcSize=8;",
}
TITLE = ("text;html=1;whiteSpace=wrap;align=center;verticalAlign=top;rounded=0;"
         "fontStyle=1;strokeColor=none;fillColor=none;fontColor=%s;")
EDGE = ("edgeStyle=orthogonalEdgeStyle;rounded=0;orthogonalLoop=1;jettySize=auto;"
        "html=1;defaultLabelBackgroundColor=page;")


def add(cells, cid, value, x, y, w, h, style):
    c = ET.SubElement(cells, "mxCell", {"id": cid, "parent": "1", "style": style,
                                        "value": value, "vertex": "1"})
    ET.SubElement(c, "mxGeometry", {"height": str(h), "width": str(w),
                                    "x": str(x), "y": str(y), "as": "geometry"})
    return c


def link(cells, eid, src, dst, color="#2F5597"):
    c = ET.SubElement(cells, "mxCell", {"id": eid, "parent": "1",
                                        "style": EDGE + "strokeColor=%s;strokeWidth=2;" % color,
                                        "edge": "1", "source": src, "target": dst})
    ET.SubElement(c, "mxGeometry", {"relative": "1", "as": "geometry"})


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    tree = ET.parse(src)
    cells = tree.getroot().find("diagram").find("mxGraphModel").find("root")
    by_id = {c.get("id"): c for c in cells.findall("mxCell")}

    # ---------- 1) 画布与原有色带 ----------
    by_id[CAM_BAND].find("mxGeometry").set("height", "1380")   # -130 → 底边 1250
    by_id[MAIN_TITLE].set("value", "线程 ① 主线程（生产者）")
    by_id[ENC_TITLE].set("value", "线程 ② 编码线程（消费者）")
    # 原有的「串口线程」框留作 ④ 色带的第一格
    g = by_id[UART_BOX].find("mxGeometry")
    g.set("x", "362"); g.set("y", "830"); g.set("width", "326"); g.set("height", "60")
    by_id[UART_BOX].set("value",
                        "50Hz 发最新结果：v2 28B + CRC16<br>无目标也发心跳（电控据此判视觉在线）")

    # ---------- 2) 图例 ----------
    add(cells, "legend",
        "【读图约定】色带 = 一个线程；框 = 步骤或文件<br>"
        "线程数：全关 1 ｜ 典型 5 ｜ 4 网页连满 9<br>"
        "②③=display_enable ④=uart_enable ⑤=log_enable",
        350, -128, 368, 90, BOX["note"])

    # ---------- 3) 线程 ④ 串口线程（左，硬件下方） ----------
    add(cells, "mcu-box", "电控 STM32（USART3 115200 8N1）<br>"
                           "视觉→电控 28B@50Hz（目标角）<br>"
                           "电控→视觉 33B@100Hz（STATE/姿态）<br>"
                           "两路独立数据流，非「请求-应答」",
        350, 682, 350, 94, BOX["mcu"])

    add(cells, "uart-band", "", 350, 780, 350, 300, STYLE_BAND["uart"])
    add(cells, "uart-title", "线程 ④ 串口线程（camera 进程内）", 360, 786, 330, 26,
        TITLE % "#7030A0")
    add(cells, "uart-s2", "poll 唤醒：数据到了立刻读（不是等 20ms）<br>解析完存快照 + 调 on_mcu",
        362, 900, 326, 55, BOX["purple"])
    add(cells, "uart-s3", "主线程只读「最近一帧」快照<br>写进 frames.csv 的 mcu_* 列 / OSD",
        362, 965, 326, 55, BOX["purple"])

    # ---------- 4) 线程 ⑤ 记录器写线程（中） ----------
    add(cells, "rec-band", "", 740, 790, 290, 460, STYLE_BAND["rec"])
    add(cells, "rec-title", "线程 ⑤ 记录器写线程", 748, 796, 274, 26,
        TITLE % "#1F6F8B")
    add(cells, "rec-1", "→ 由主线程调用 on_frame()<br>每帧一行（32 列：视觉 + 电控 9 列）",
        752, 845, 266, 55, BOX["teal"])
    add(cells, "rec-2", "→ 由串口线程调用 on_mcu()<br>可选：额外存 100Hz 原始流（默认关）",
        752, 910, 266, 55, BOX["teal"])
    add(cells, "rec-3", "环形缓冲 2048 行（≈11s，预录 3s）<br>"
                         "STATIC→FLIGHT 开段<br>FLIGHT→STOP 再记 2s 收尾",
        752, 975, 266, 78, BOX["teal2"])
    add(cells, "rec-4", "本线程：20ms / 64 行 write<br>每秒 fdatasync（采集线程不碰 I/O）",
        752, 1063, 266, 60, BOX["teal"])
    add(cells, "rec-5", "logs/sess_*/<br>frames.csv（含电控）· meta.txt<br>"
                         "+ health_*.csv · video_frames_*.csv<br>分析：tools/flight_merge.py",
        752, 1133, 266, 105, BOX["teal"])

    # ---------- 5) 线程 ③ HTTP（右） ----------
    add(cells, "http-band", "", 1050, 790, 460, 170, STYLE_BAND["http"])
    add(cells, "http-title", "线程 ③ HTTP 预览线程", 1060, 796, 440, 26,
        TITLE % "#2E7D6B")
    add(cells, "http-1", "服务线程：accept 连接，最多 4 个客户端槽（满了回 503）",
        1062, 833, 436, 50, BOX["green"])
    add(cells, "http-2", "客户端线程 ×4（每个网页连接 1 个）<br>MJPEG / 抓图，每客户端限 20fps",
        1062, 893, 436, 55, BOX["green"])

    # ---------- 6) 诊断（右下，注明不开线程） ----------
    add(cells, "diag-band", "", 1050, 980, 460, 270, STYLE_BAND["diag"])
    add(cells, "diag-title", "运行状态与退出归因（diag：不开线程）", 1060, 986, 440, 26,
        TITLE % "#60497A")
    add(cells, "diag-1", "print_* 9 个分类开关（默认全开，总开关 print_enable）",
        1062, 1022, 436, 42, BOX["diag"])
    add(cells, "diag-2", "state/run_history.log<br>"
                          "supervisor_start → start → ready → alive(30s) → exit / crash",
        1062, 1072, 436, 52, BOX["diag"])
    add(cells, "diag-3", "崩溃处理器：信号号 + 帧数 + 温度（只 open/write）",
        1062, 1132, 436, 42, BOX["diag"])
    add(cells, "diag-4", "孤儿保护 PR_SET_PDEATHSIG + status.sh 一条命令复核",
        1062, 1182, 436, 42, BOX["diag"])

    # ---------- 7) 两个小注释：侧车文件 + OSD ----------
    add(cells, "vs-box", "video_frames_*.csv<br>AVI 第 k 帧 → frame_id",
        1240, 600, 176, 50, BOX["teal"])
    add(cells, "osd-note", "④b OSD（可选）：编码前把电控状态烧进 Y 平面<br>"
                           "→ 网页 / 录像 / 抓图都能看到",
        770, 704, 250, 34, BOX["note"])

    # ---------- 8) 连线 ----------
    link(cells, "e-main-rec", PENDING, "rec-1", "#1F6F8B")
    link(cells, "e-uart-rec", "uart-s3", "rec-2", "#1F6F8B")
    link(cells, "e-uart-mcu", UART_BOX, "mcu-box", "#C55A11")
    link(cells, "e-uart-2", UART_BOX, "uart-s2", "#7030A0")
    link(cells, "e-uart-3", "uart-s2", "uart-s3", "#7030A0")
    link(cells, "e-avi-side", AVI_BOX, "vs-box", "#548235")
    link(cells, "e-jpeg-http", HTTP_READ, "http-1", "#2E7D6B")
    link(cells, "e-http-2", "http-1", "http-2", "#2E7D6B")

    tree.write(dst, encoding="utf-8", xml_declaration=False)
    print("wrote", dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
