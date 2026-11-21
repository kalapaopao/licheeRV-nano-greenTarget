#!/usr/bin/env python3
"""UART 内部自环测试：证明这颗 UART 的"收"通路（驱动+FIFO+控制器）本身是好的。

原理：8250/dw-apb-uart 的 MCR 寄存器有 LOOP 位，置位后芯片内部把 TX 接到 RX，
不经过任何引脚。所以：
    自环能收到自己发的字节  → UART 收通路正常，问题一定在"引脚/焊盘/复用"
    自环也收不到            → UART 收通路本身有问题（极少见）

用法（板端）：python3 uart_loopback.py [/dev/ttyS1]
"""
import fcntl
import os
import struct
import sys
import termios
import time
import tty

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyS1"
TIOCM_LOOP = 0x8000   # Linux asm-generic/termios.h；Python termios 没导出这个名字
PAYLOAD = bytes(range(0x30, 0x30 + 64))      # 64 字节可打印字符

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B115200
a[2] &= ~termios.CRTSCTS
a[0] &= ~(termios.IXON | termios.IXOFF)
termios.tcsetattr(fd, termios.TCSANOW, a)

def rx_count():
    """从 /proc/tty/driver/serial 读该口的内核 rx 计数"""
    port = DEV.rsplit("ttyS", 1)[-1]
    with open("/proc/tty/driver/serial") as f:
        for line in f:
            if line.startswith(port + ":"):
                for tok in line.split():
                    if tok.startswith("rx:"):
                        return int(tok[3:])
    return -1

# 清掉残留字节
try:
    while os.read(fd, 4096):
        pass
except BlockingIOError:
    pass

before = rx_count()
print("%s 自环前 rx=%d" % (DEV, before))

# 打开内部自环（TIOCM_LOOP）
fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("i", TIOCM_LOOP))
time.sleep(0.05)

os.write(fd, PAYLOAD)
time.sleep(0.3)                              # 64 字节 @115200 ≈ 5.6ms，留足余量

got = b""
try:
    while True:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        got += chunk
except BlockingIOError:
    pass

after = rx_count()

# 关掉自环
fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("i", TIOCM_LOOP))
terms = termios.tcgetattr(fd)
os.close(fd)

print("%s 自环后 rx=%d（内核计数 +%d）" % (DEV, after, after - before))
print("回读 %d 字节：%s" % (len(got), got[:64].hex(" ") if got else "(空)"))
print()
ok_recv = len(got) >= len(PAYLOAD) and PAYLOAD in got
ok_cnt = after > before
if ok_recv and ok_cnt:
    print("=> 通过：UART 收通路（驱动 + FIFO + 控制器）完全正常")
    print("   所以问题只可能在【引脚 / 焊盘 / 引脚复用】，不在软件、不在波特率")
elif ok_cnt:
    print("=> 部分通过：内核 rx 计数增长，但回读内容不符（可能有别的数据混入）")
else:
    print("=> 失败：内部自环都收不到 → 这颗 UART 的接收控制器/驱动有问题")
