#!/usr/bin/env python3
"""生成「线程间数据流与共享结构」图：每个共享对象谁写、谁读、用什么锁、多大、怎么赋值。

和 row_life.png（一行数据的一生）互补：那张讲"时间"，这张讲"空间"——
线程之间到底传了什么、传的是拷贝还是指针、锁保护哪一步。

用法：python tools/gen_thread_dataflow.py docs/architecture/thread_dataflow.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "C:/Windows/Fonts/msyh.ttc"
W, H = 1640, 1080
S = 2
BG = (0xFA, 0xFB, 0xFD)
INK = (0x22, 0x26, 0x2B)
GREY = (0x88, 0x8E, 0x96)
PURPLE = (0x70, 0x30, 0xA0)
BLUE = (0x2F, 0x55, 0x97)
GREEN = (0x2E, 0x7D, 0x6B)
TEAL = (0x1F, 0x6F, 0x8B)
ORANGE = (0xC5, 0x5A, 0x11)

LX, LW = 40, 240          # 左列（写者）
SX, SW = 470, 570         # 中列（共享对象）
RX, RW = 1160, 240        # 右列（读者）
NX, NW = 1420, 200        # 右注

ROWS = [
    dict(y=140, lthread="串口线程\n(100Hz)", rthread="主线程\n(80Hz)",
         obj="电控快照 mailbox（深度 1，最新值覆盖）",
         fields="McuState last_mcu_   (seq,state,flags,roll,pitch,yaw,gx,gy,gz)\n"
                "MonoUs  last_mcu_rx_us_   ← 这一帧的到达时刻\n"
                "锁：mcu_mu_   共 ≈32 字节   持有 几十纳秒",
         wop="① 整体赋值（加锁）", rop="② 拷到栈上（加锁）", color=PURPLE,
         note="拷贝是 32 字节的结构体，\n不是图像\n读不到就返回 nullptr\n（那几列写 nan）"),
    dict(y=310, lthread="主线程\n(80Hz 取帧后)", rthread="编码线程\n(尽力)",
         obj="待编码帧交接台（深度 1，新帧覆盖旧帧）",
         fields="VIDEO_FRAME_INFO_S pending（描述符，不是像素）\n"
                "uint32_t pending_frame_id\n"
                "锁：input_mutex   满了先 ReleaseChnFrame 旧帧 → preview_replaced++",
         wop="① 拷描述符（加锁）", rop="② 取走+清标志", color=BLUE,
         note="像素一直在 DRAM 里不动\n传的是「物理地址+格式」\n所以丢帧只丢描述符"),
    dict(y=480, lthread="编码线程\n(VENC 出 JPEG)", rthread="HTTP 线程\n(预览/抓图)",
         obj="最新一份 JPEG（指针交换，O(1)）",
         fields="std::vector<unsigned char> latest（几十 KB，堆上）\n"
                "unsigned long generation ← 版本号（有没有新帧）\n"
                "锁：jpeg_mutex  写者 swap()，读者整份 copy",
         wop="① swap 指针（加锁）", rop="② copy 一份（加锁）", color=GREEN,
         note="录像不拷贝：编码线程\n直接把这份字节写进 AVI\n（一份 JPEG 三处复用）"),
    dict(y=650, lthread="主线程 + 串口线程\n(两个生产者)", rthread="记录器写线程\n(自己节拍)",
         obj="记录器环形缓冲（2048 行，每行已拼好的文本）",
         fields="Row ring_[2048]  每行 ≤256B（text[256]+ts_us+kind）\n"
                "head_ / tail_ / count_\n"
                "锁：mu_ + 条件变量 cv_（wait_for 20ms / 64 行）",
         wop="① memcpy 行（加锁）", rop="② 取出 → write()", color=TEAL,
         note="满时：待机丢最旧、\n会话中丢最新并 dropped++\n生产者从不等消费者"),
    dict(y=820, lthread="各线程", rthread="各线程 / status.sh",
         obj="计数器：原子（无锁）+ 锁内普通变量",
         fields="原子：running_ quit_ sent_frames_ send_errors_ encoded errors\n"
                "原子：network_sent/bytes dropped_ rows_* trigger_us_ land_us_\n"
                "锁内：rx_frames_ rx_crc_errors_（在 mcu_mu_ 里读，不是原子）",
         wop="++ / store", rop="load（日志/meta）", color=ORANGE,
         note="只做统计，不参与决策\n所以不需要锁，也不会阻塞"),
]


def load(size):
    for p in (FONT, "C:/Windows/Fonts/simhei.ttf"):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def main() -> int:
    out = sys.argv[1]
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = load(26)
    f_head = load(15)
    f_obj = load(16)
    f_field = load(12)
    f_op = load(12)
    f_thread = load(14)

    def T(x, y, s, f, fill=INK, anchor="la"):
        d.text((x, y), s, font=f, fill=fill, anchor=anchor)

    T(LX, 24, "线程间数据流与共享结构：谁写、谁读、锁保护哪一步", f_title)
    T(LX, 60, "五条共享通道 + 一组原子计数。所有「拷贝」都只拷几十字节的小结构体，图像像素从不拷贝。",
      f_head, GREY)

    # 列标题
    T(LX + LW / 2, 104, "写入方", f_head, GREY, anchor="ma")
    T(SX + SW / 2, 104, "共享对象（锁 + 大小 + 赋值方式）", f_head, GREY, anchor="ma")
    T(RX + RW / 2, 104, "读取方", f_head, GREY, anchor="ma")

    for r in ROWS:
        y = r["y"]
        bh = 130
        # 左：写者
        d.rounded_rectangle([LX, y, LX + LW, y + bh], radius=10, fill=(255, 255, 255),
                            outline=r["color"], width=2)
        lt = (r["lthread"].split("\n") + [""])[:2]
        rt = (r["rthread"].split("\n") + [""])[:2]
        T(LX + LW / 2, y + 22, lt[0], f_thread, r["color"], anchor="ma")
        T(LX + LW / 2, y + 46, lt[1], f_field, GREY, anchor="ma")
        T(LX + LW / 2, y + 74, "写", f_thread, r["color"], anchor="ma")

        # 中：共享对象
        d.rounded_rectangle([SX, y, SX + SW, y + bh], radius=10,
                            fill=(0xFF, 0xFF, 0xFF), outline=r["color"], width=3)
        T(SX + 14, y + 12, r["obj"], f_obj, r["color"])
        for i, line in enumerate(r["fields"].split("\n")):
            T(SX + 14, y + 40 + i * 24, line, f_field, INK)

        # 右：读者
        d.rounded_rectangle([RX, y, RX + RW, y + bh], radius=10, fill=(255, 255, 255),
                            outline=r["color"], width=2)
        T(RX + RW / 2, y + 22, rt[0], f_thread, r["color"], anchor="ma")
        T(RX + RW / 2, y + 46, rt[1], f_field, GREY, anchor="ma")
        T(RX + RW / 2, y + 74, "读", f_thread, r["color"], anchor="ma")

        # 箭头 + 操作说明
        mid = y + bh / 2
        d.line([(LX + LW, mid), (SX, mid)], fill=r["color"], width=3)
        d.polygon([(SX, mid), (SX - 12, mid - 6), (SX - 12, mid + 6)], fill=r["color"])
        T((LX + LW + SX) / 2, mid - 26, r["wop"], f_op, r["color"], anchor="ma")
        d.line([(SX + SW, mid), (RX, mid)], fill=r["color"], width=3)
        d.polygon([(RX, mid), (RX - 12, mid - 6), (RX - 12, mid + 6)], fill=r["color"])
        T((SX + SW + RX) / 2, mid - 26, r["rop"], f_op, r["color"], anchor="ma")

        # 右注
        for i, line in enumerate(r["note"].split("\n")):
            T(NX, y + 24 + i * 22, line, f_field, GREY)

    # 底部：三条纪律
    d.rounded_rectangle([LX, 1010, W - 40, H - 16], radius=10, fill=(255, 255, 255),
                        outline=(0xD0, 0xD5, 0xDC), width=2)
    T(LX + 16, 1030, "三条纪律：① 锁内只做赋值/memcpy，绝不 write()、printf、malloc；② 锁只保护「一次完整赋值」，"
                    "永远不给跨线程共享指针；③ 时间戳由写者在同一把锁里顺手记下，读者一起拷走。",
      f_field, INK)

    img = img.resize((W * S, H * S), Image.LANCZOS)
    img.save(out)
    print("saved", out, img.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
