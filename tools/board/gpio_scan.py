#!/usr/bin/env python3
"""只读：高速采样 4 个 GPIO bank 的电平，找出哪些引脚真实在跳变。

原理：SG2002 的 GPIO 数据寄存器读出来是"引脚真实电平"（我们已用 console 验证：
往 UART0 狂发字节时，bit A14 在跳）。所以只要采样这四个 bank，
哪个 bit 在跳，就说明那根引脚上确实有信号。

用法（板端）：
    python3 gpio_scan.py            # 采 3 秒
    python3 gpio_scan.py 5          # 采 5 秒
"""
import mmap
import os
import struct
import sys
import time

BANKS = [("A", 0x03020000), ("B", 0x03021000), ("C", 0x03022000), ("D", 0x03023000)]
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
PAGE = 0x1000

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
maps = {}
for name, base in BANKS:
    off = base & ~(PAGE - 1)
    m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)
    maps[name] = (m, base - off)

def read_word(name):
    m, delta = maps[name]
    return struct.unpack_from("<I", m, delta)[0]

# 先读一次作为基准
prev = {n: read_word(n) for n, _ in BANKS}
toggles = {n: [0] * 32 for n, _ in BANKS}
samples = 0
t0 = time.time()
while time.time() - t0 < SECONDS:
    for n, _ in BANKS:
        v = read_word(n)
        d = v ^ prev[n]
        if d:
            for b in range(32):
                if (d >> b) & 1:
                    toggles[n][b] += 1
            prev[n] = v
    samples += 1

elapsed = time.time() - t0
print("采样 %d 轮 / %.2f 秒（约 %.0f 轮/秒），只读，未写任何寄存器" % (samples, elapsed, samples / elapsed))
print()
found = False
for n, _ in BANKS:
    hits = [(b, c) for b, c in enumerate(toggles[n]) if c > 0]
    if not hits:
        print("  GPIO%s: 没有引脚跳变" % n)
        continue
    found = True
    print("  GPIO%s: 以下引脚在跳变" % n)
    for b, c in hits:
        print("      GPIO%s%-2d   跳变 %d 次%s" % (n, b, c, "   ←←← 信号在这里" if c > 20 else ""))
print()
if not found:
    print("结论：4 个 bank 全部静止 → 电控的信号没有到达 SoC 的任何 GPIO 引脚")
else:
    print("结论：上面列出的引脚确实有信号。把它和 UART1_RX 对上就能收数据。")
os.close(fd)
