# -*- coding: utf-8 -*-
"""生成 docs/architecture/ 下的架构图（Pillow 绘制，无需 draw.io）。

用法：
    python tools/draw_architecture.py
输出：
    docs/architecture/10_总览与内存布局.png
    docs/architecture/11_主线程逐帧流水线.png
    docs/architecture/12_帧的借还与所有权.png
    docs/architecture/13_线程同步与检测算法.png

改图只需改本文件的布局函数，不用重新装 draw.io。
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "architecture")
FONT_R = "C:/Windows/Fonts/msyh.ttc"
FONT_B = "C:/Windows/Fonts/msyhbd.ttc"
W = 2400

C = {
    "blue":   ("#DCE9F8", "#2F5597"),
    "green":  ("#E2EFDA", "#548235"),
    "orange": ("#FCE4D6", "#C55A11"),
    "purple": ("#E8E1F2", "#7030A0"),
    "gray":   ("#F2F2F2", "#808080"),
    "red":    ("#FBE0E0", "#C00000"),
    "yellow": ("#FFF2CC", "#BF9000"),
    "white":  ("#FFFFFF", "#BFBFBF"),
}


def font(size, bold=False):
    return ImageFont.truetype(FONT_B if bold else FONT_R, size)


def wrap(text, fnt, max_w):
    lines, cur = [], ""
    for ch in text:
        if ch == "\n":
            lines.append(cur); cur = ""; continue
        if fnt.getlength(cur + ch) <= max_w:
            cur += ch
        else:
            lines.append(cur); cur = ch
    if cur:
        lines.append(cur)
    return lines


def rbox(d, x, y, w, h, fill, outline, radius=16, width=3):
    d.rounded_rectangle([x, y, x + w, y + h], radius=radius, fill=fill, outline=outline, width=width)


def box(d, x, y, w, h, title, lines, color="blue", tsize=30, bsize=23, pad=18, align="left", tag=None):
    fill, outline = C[color]
    rbox(d, x, y, w, h, fill, outline)
    cy = y + pad
    if tag:
        tf = font(20, True)
        d.text((x + pad, cy), tag, font=tf, fill=outline)
        cy += 30
    d.text((x + pad, cy), title, font=font(tsize, True), fill="#1F1F1F")
    cy += tsize + 10
    for ln in lines:
        for part in wrap(ln, font(bsize), w - 2 * pad):
            d.text((x + pad, cy), part, font=font(bsize), fill="#333333")
            cy += int(bsize * 1.45)
        cy += 4
    return y + h


def note(d, x, y, w, h, title, lines, color="gray", tsize=26, bsize=21):
    fill, outline = C[color]
    rbox(d, x, y, w, h, fill, outline, radius=12, width=2)
    cy = y + 14
    d.text((x + 16, cy), title, font=font(tsize, True), fill=outline)
    cy += tsize + 8
    for ln in lines:
        for part in wrap(ln, font(bsize), w - 32):
            d.text((x + 16, cy), part, font=font(bsize), fill="#333333")
            cy += int(bsize * 1.4)
    return y + h


def note_h(w, title, lines, tsize=26, bsize=21):
    """按内容估算说明框高度，避免大片空白。"""
    n = 0
    for ln in lines:
        n += len(wrap(ln, font(bsize), w - 32))
    return 14 + tsize + 8 + int(n * bsize * 1.4) + 14


def note_fit(d, x, y, w, title, lines, color="gray", tsize=26, bsize=21):
    h = note_h(w, title, lines, tsize, bsize)
    return note(d, x, y, w, h, title, lines, color, tsize, bsize)


def arrow_v(d, x, y0, y1, color="#2F5597", width=4, label=None):
    d.line([x, y0, x, y1 - 12], fill=color, width=width)
    d.polygon([(x - 9, y1 - 14), (x + 9, y1 - 14), (x, y1)], fill=color)
    if label:
        d.text((x + 14, (y0 + y1) // 2 - 12), label, font=font(20), fill=color)


def arrow_h(d, x0, x1, y, color="#2F5597", width=4, label=None):
    d.line([x0, y, x1 - 12, y], fill=color, width=width)
    d.polygon([(x1 - 14, y - 9), (x1 - 14, y + 9), (x1, y)], fill=color)
    if label:
        d.text(((x0 + x1) // 2 - 20, y - 30), label, font=font(20), fill=color)


def header(d, title, subtitle):
    d.rectangle([0, 0, W, 120], fill="#1F3864")
    d.text((60, 28), title, font=font(40, True), fill="white")
    d.text((62, 82), subtitle, font=font(22), fill="#C9D6EA")


def canvas(h):
    img = Image.new("RGB", (W, h), "white")
    return img, ImageDraw.Draw(img)


# ---------------------------------------------------------------- 10 总览
def fig10():
    h = 1430
    img, d = canvas(h)
    header(d, "① 总览：从传感器到你的进程", "硬件取流 → 缓存映射 → 抽样 → 检测/跟踪 → 角度 → 网页 / 串口")

    # 第一行：取流
    y = 190
    boxes = [
        ("GC4653 传感器", ["1280×720 @ 80 fps", "720P 寄存器表 + 36 MHz CAM PLL", "用户态驱动（项目自带）"], "red"),
        ("VI 硬件取帧", ["NV21 三平面", "Y 921600 B + UV 460800 B", "CVI_VI_GetChnFrame"], "blue"),
        ("VB 池", ["SAMPLE_PLAT_SYS_INIT 按 720P 分配", "固定 4 块，物理地址反复复用"], "orange"),
        ("缓存映射", ["CVI_SYS_MmapCache(phy,len)", "VaCache[16] 命中复用（map_miss=16）", "未缓存实测 223 ms/帧"], "green"),
    ]
    bw, gap = 540, 40
    for i, (t, ls, c) in enumerate(boxes):
        x = 60 + i * (bw + gap)
        box(d, x, y, bw, 210, t, ls, c)
        if i:
            arrow_h(d, x - gap + 8, x - 6, y + 105)

    # 第二行：处理
    y2 = 470
    boxes2 = [
        ("4× 抽样到 320×180", ["每 S×S 块取 ns×ns 最大值", "ns=1 保 80 fps（采样 ≈4.9 ms）", "Y/UV 三平面缓存复用"], "orange"),
        ("绿色检测 detectMulti", ["YUV 阈值 → 3×3 开运算", "8 连通域 → 形状过滤", "输出≤8 个候选（含质量分）"], "orange"),
        ("筛选 + 卡尔曼", ["评分 = 质量 + 离预测距离", "角度域 2 状态（角度/角速度）", "confirmed / coasting / lost"], "purple"),
        ("角度 + 瞄点", ["atan 针孔模型 + 光轴补偿", "瞄准点 = 灯上移 k×灯直径", "输出 yaw/pitch/vyaw/vpitch/dist"], "green"),
    ]
    for i, (t, ls, c) in enumerate(boxes2):
        x = 60 + i * (bw + gap)
        box(d, x, y2, bw, 210, t, ls, c)
        if i:
            arrow_h(d, x - gap + 8, x - 6, y2 + 105)

    # 第三行：出口
    y3 = 750
    outs = [
        ("发布最新快照", ["只保留最新一份结果（不排队）", "无目标也发心跳帧"], "blue"),
        ("串口线程 50 Hz", ["v2: 28 B + CRC16（推荐）", "v1: ASCII $GT,...（调试用）", "无目标：detected=0 / dist=-1"], "blue"),
        ("编码 → 网页", ["硬件 JPEG（CPU 零占用）", "8081：MJPEG + /snapshot.jpg", "检测框叠加在 Y 平面"], "blue"),
        ("录像 / 日志", ["可选 AVI（record_enable）", "每 5 秒统计 + GREEN_TARGET 10 Hz"], "gray"),
    ]
    ow = 540
    for i, (t, ls, c) in enumerate(outs):
        x = 60 + i * (ow + gap)
        box(d, x, y3, ow, 200, t, ls, c)
    # 第二行末 → 第三行首 的折线
    d.line([60 + 3 * (bw + gap) + bw // 2, y2 + 210, 60 + 3 * (bw + gap) + bw // 2, y3 - 40, 60 + ow // 2, y3 - 40], fill="#2F5597", width=4)
    d.line([60 + ow // 2, y3 - 40, 60 + ow // 2, y3 - 6], fill="#2F5597", width=4)
    d.polygon([(60 + ow // 2 - 9, y3 - 8), (60 + ow // 2 + 9, y3 - 8), (60 + ow // 2, y3)], fill="#2F5597")

    # 关键数字
    b1 = note_fit(d, 60, 1000, 1130, "关键实测数字（1280×720，板端）", [
        "采集 78~82 fps；检测 5.4~5.9 ms/帧（抽样 4.9~5.3 + 跟踪 0.4~0.6）",
        "编码 ≈79 fps；取帧→开始写串口 平均 11.5 ms，最大 16~28 ms",
        "开机自启后约 15~20 s 出画面；内存可用约 81 MB",
        "温度：空闲 57°C，满负荷 105 s 内 66.8→68.9°C（≥74.5°C 保护停机）",
    ], "green")

    b2 = note_fit(d, 1210, 1000, 1130, "设计上的三条硬约束", [
        "① 只保留最新结果：不排队，避免旧数据累积延迟；",
        "② 检测内联在主线程：5 ms 塞得进 12.5 ms 帧预算，拆线程只增加锁开销；",
        "③ 异步线程只拿 POD 拷贝：绝不把 VIDEO_FRAME_INFO_S 交给别的线程。",
    ], "yellow")

    note(d, 60, max(b1, b2) + 30, 2280, 110, "一句话", [
        "传感器每 12.5 ms 交一帧给 VI；进程用缓存映射直接读物理内存，抽 1/16 的点做检测，"
        "把“目标相对光轴的角误差”算出来后，按 50 Hz 发给电控，同时把同一帧交给硬件编码器做网页预览。",
    ], "blue", tsize=24, bsize=22)

    img.save(os.path.join(OUT, "10_总览与内存布局.png"))


# ---------------------------------------------------------------- 11 主线程逐帧
def fig11():
    steps = [
        ("1", "CVI_VI_GetChnFrame(0,0,&frame,1000)", "超时 → ++timeouts，continue（不借用该帧）", "blue"),
        ("2", "建立 FrameTiming", "frame_id / 原始 PTS / capture_mono_us（取帧即记，非曝光时刻）", "green"),
        ("3", "预热期过滤：t-start<4 s", "Release + continue（ISP/AE 尚未稳定）", "gray"),
        ("4", "一次性初始化（t-start>2 s）", "AE 曝光/增益、黑电平设置与读回校验 → 失败 tuning_failed=1 → running=0", "orange"),
        ("5", "首帧打印 VI 几何", "分辨率 / stride / length / 像素格式", "gray"),
        ("6", "虚拟地址缓存映射", "pu8VirAddr 为空 → map_cached()（VaCache 16 项）", "green"),
        ("7", "4× 抽样到 320×180", "块内 ns×ns 取最大值（ns=1 时 4.9 ms）", "orange"),
        ("8", "detectMulti() 收集候选", "YUV 阈值 → 开运算 → 8 连通域 → 形状过滤（≤8 个）", "orange"),
        ("9", "评分选目标 + 卡尔曼", "质量分与离预测距离加权；角度域 2 状态；confirm/coast/lost", "purple"),
        ("10", "角度 + 光轴补偿 + 瞄点偏移", "atan → 减光轴补偿 → 瞄点上移 k×灯直径 → 输出 yaw/pitch/vyaw/vpitch/dist", "purple"),
        ("11", "画框（display_on 且命中）", "网格坐标 ×S 还原；Y 平面写 3 px 白框；IonFlushCache 写回", "blue"),
        ("12", "日志（限频）", "GREEN_TARGET ≤10 Hz（含 frame= 与灯中心角度）", "gray"),
        ("13", "publish(tgt, timing) → 串口线程", "只做一次结构体拷贝；串口线程 50 Hz 发最新快照", "blue"),
        ("14", "帧交接", "display_on → pending 槽（覆盖旧帧 preview_replaced++）；否则直接 Release", "blue"),
    ]
    h = 2400
    img, d = canvas(h)
    header(d, "② 主线程逐帧流水线", "每 12.5 ms 一帧；括号内为当前实现要点（以源码为准，不绑定行号）")

    x, w = 70, 1560
    y = 170
    for i, (num, title, detail, color) in enumerate(steps):
        fill, outline = C[color]
        rbox(d, x, y, w, 120, fill, outline)
        d.ellipse([x + 18, y + 30, x + 78, y + 90], fill=outline)
        d.text((x + 38, y + 44), num, font=font(28, True), fill="white")
        d.text((x + 100, y + 18), title, font=font(28, True), fill="#1F1F1F")
        for k, part in enumerate(wrap(detail, font(22), w - 130)):
            d.text((x + 100, y + 60 + k * 32), part, font=font(22), fill="#333333")
        if i < len(steps) - 1:
            arrow_v(d, x + w // 2, y + 120, y + 150)
        y += 150

    # 右侧说明
    nx, nw, y = 1690, 640, 170
    for title, lines, color in [
        ("耗时预算（实测）", [
            "抽样 ≈4.9 ms    跟踪 ≈0.5 ms",
            "检测合计 5.4~5.9 ms / 帧",
            "帧预算 12.5 ms → 余量约 6.5 ms",
            "串口/编码/网页都在别的线程，不占主线程",
        ], "green"),
        ("分支与开关", [
            "detect_enable=false → 跳过 7~13 步（纯取流）",
            "display_enable=false → 不建 VENC/编码线程/HTTP",
            "detect_stride=N → 每 N 帧才做一次检测",
            "DETECT_SAMPLE_STEPS=1/2/4 → 采样密度与帧率权衡",
        ], "yellow"),
        ("失败与保护", [
            "取帧失败：只计 timeouts，不退出",
            "ISP 调参失败：tuning_failed=1 → running=0",
            "编码连续错误≥5：running=0",
            "温度保护由 start.sh 负责（≥74.5°C 停机）",
        ], "red"),
        ("为什么这样排", [
            "检测必须紧跟取帧：越晚处理，结果越旧；",
            "只保留最新结果：宁可丢旧帧，不要排队；",
            "所有异步只拿 POD：帧缓冲的生命周期留在主线程。",
        ], "blue"),
        ("如何用日志核对每一步", [
            "第 6 步：首次出现 map_miss=16；",
            "第 7~9 步：DETECT 行的 sample/track 耗时；",
            "第 13 步：UART 行 rate≈50 Hz、errors=0；",
            "第 14 步：preview_replaced 持续增长属正常。",
        ], "gray"),
    ]:
        y = note_fit(d, nx, y, nw, title, lines, color) + 24

    img.save(os.path.join(OUT, "11_主线程逐帧流水线.png"))


# ---------------------------------------------------------------- 12 帧所有权
def fig12():
    h = 1400
    img, d = canvas(h)
    header(d, "③ 帧的借还与所有权", "VI 帧来自固定 VB 池：谁借出、谁释放、失败路径怎么走")

    box(d, 80, 190, 620, 190, "① 借出", ["CVI_VI_GetChnFrame 成功后", "该帧归本进程使用", "用毕必须 Release，否则 VB 池耗尽"], "blue")
    box(d, 800, 190, 620, 190, "② 主线程处理", ["检测/跟踪/画框都在借来的帧上做", "只拷贝结构体给其它线程（不拷贝像素）", "绝不把 VIDEO_FRAME_INFO_S 传给异步线程"], "green")
    box(d, 1520, 190, 800, 190, "③ 两条交接路径", ["display_enable=false：直接 ReleaseChnFrame", "display_enable=true：放入 pending 槽（深度 1）"], "orange")
    arrow_h(d, 700, 792, 285)
    arrow_h(d, 1420, 1512, 285)

    box(d, 1520, 450, 800, 170, "④ pending 槽（深度 1）", ["新帧覆盖旧帧：preview_replaced++", "意义：永远编码“最新一帧”，不积压延迟"], "purple")
    arrow_v(d, 1920, 380, 444)

    box(d, 1520, 690, 800, 190, "⑤ 编码线程", ["取走 pending → CVI_VENC_SendFrame", "GetStream 取 JPEG 包 → 拷贝数据", "CVI_VENC_StopRecvFrame + ReleaseChnFrame"], "blue")
    arrow_v(d, 1920, 620, 684)

    box(d, 1520, 950, 800, 170, "⑥ 帧归还", ["Release 后帧回到 VB 池，可再次被 VI 填充", "不释放 = 跑几十帧后取帧超时"], "green")
    arrow_v(d, 1920, 880, 944)

    box(d, 80, 450, 1340, 210, "失败与退出路径", [
        "GetChnFrame 失败：没有借到帧，直接 continue（不会泄漏）；",
        "VENC 发送失败：仍会 ReleaseChnFrame，避免漏还；",
        "退出：先停串口 → join 编码线程 → 释放 pending → 销毁 VENC/VI → SYS_Exit。",
    ], "red")

    b = note_fit(d, 80, 720, 1340, "为什么 pending 只留 1 帧", [
        "编码比检测慢（JPEG 编码 ≈12.5 ms/帧，网页客户端另有 20 fps 限速），",
        "如果排队，编码发出去的就是几帧前的画面，网页延迟会越积越大；",
        "覆盖旧帧只丢“预览用的帧”，不影响检测与串口输出。",
    ], "yellow")

    b = note_fit(d, 80, b + 30, 1340, "实测对照", [
        "preview_replaced 持续增长 = 编码确实跟不上检测（正常现象，不是错误）；",
        "取帧超时 times=0 且 VB 池未耗尽 = 释放路径正确；",
        "板端日志 `stopped frames=... encoded=...` 两者差值即被覆盖的帧数。",
    ], "green")

    note(d, 1520, 1150, 800, 120, "一句话", ["借出来的帧必须还；异步线程不碰帧，只碰拷贝。"], "blue", tsize=24, bsize=23)

    img.save(os.path.join(OUT, "12_帧的借还与所有权.png"))


# ---------------------------------------------------------------- 13 线程同步与检测算法
def fig13():
    h = 1450
    img, d = canvas(h)
    header(d, "④ 线程同步与检测算法", "五个线程共享什么、用什么锁；检测算法内部怎么走")

    # 左：线程与共享状态
    d.text((70, 160), "线程与共享状态", font=font(30, True), fill="#1F3864")
    rows = [
        ("主线程", "检测/跟踪/发布；持有 pending 槽写入权", "green"),
        ("编码线程", "读 pending（input_mutex）；写 latest/generation（jpeg_mutex）", "blue"),
        ("HTTP 线程×N", "读 latest（jpeg_mutex）；每客户端独立，限速 ~20 fps", "blue"),
        ("串口发送线程", "读最新快照（UartOut::mu_）；写年龄统计（write_age_mu_）", "purple"),
        ("串口接收线程", "解析下行帧；只写接收缓冲，不阻塞发送", "orange"),
    ]
    y = 210
    for name, desc, color in rows:
        fill, outline = C[color]
        rbox(d, 70, y, 1080, 130, fill, outline)
        d.text((92, y + 18), name, font=font(26, True), fill="#1F1F1F")
        for k, part in enumerate(wrap(desc, font(22), 1000)):
            d.text((92, y + 58 + k * 30), part, font=font(22), fill="#333333")
        y += 150

    note_fit(d, 70, y + 10, 1080, "锁的约束（当前实现）", [
        "jpeg_mutex：只在“换图/取图”时短暂持有，拷贝后立刻释放；",
        "input_mutex：主线程与编码线程之间只保护 pending 槽；",
        "UartOut::mu_ / write_age_mu_：两把锁不嵌套，避免死锁；",
        "串口线程绝不回调主线程，也不持有帧引用。",
    ], "gray")

    # 右：检测算法
    x2 = 1230
    d.text((x2, 160), "检测算法（纯传统视觉，无 NPU）", font=font(30, True), fill="#1F3864")
    steps = [
        ("输入", "320×180 网格：Y / U / V 三平面"),
        ("颜色判据", "Y>阈值、U/V<128（偏绿）、饱和度达标"),
        ("去噪", "3×3 形态学开运算（先腐蚀后膨胀）"),
        ("连通域", "8 邻域显式栈标记，统计面积/包围盒/质心"),
        ("形状过滤", "面积 2~400、填充率≥0.5、长宽比≤2.2、半径≤12"),
        ("候选评分", "亮度+饱和度质量分（detectMulti 返回≤8 个）"),
        ("跟踪", "加权选目标 → 卡尔曼 → confirmed/coast/lost"),
    ]
    y = 210
    for i, (t, desc) in enumerate(steps):
        fill, outline = C["orange" if i < 6 else "purple"]
        rbox(d, x2, y, 1070, 118, fill, outline)
        d.text((x2 + 20, y + 14), t, font=font(25, True), fill="#1F1F1F")
        for k, part in enumerate(wrap(desc, font(21), 1010)):
            d.text((x2 + 20, y + 50 + k * 28), part, font=font(21), fill="#333333")
        if i < len(steps) - 1:
            arrow_v(d, x2 + 40, y + 118, y + 142, color="#C55A11", width=3)
        y += 142

    note_fit(d, x2, y + 10, 1070, "状态机与输出", [
        "confirmed（连续命中≥5 帧）→ 可闭环；",
        "coasting（短时跟丢≤10 帧）→ 继续输出预测值；",
        "lost → detected=0、角度清零、dist=-1；",
        "输出前再叠加光轴补偿与“灯→装甲板”的瞄点偏移。",
    ], "green")

    img.save(os.path.join(OUT, "13_线程同步与检测算法.png"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    fig10(); fig11(); fig12(); fig13()
    print("saved to", OUT)
