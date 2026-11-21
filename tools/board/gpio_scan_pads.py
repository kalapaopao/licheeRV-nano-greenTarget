#!/usr/bin/env python3
"""读 4 个 GPIO bank 的「外部引脚电平」寄存器（偏移 0x50），找出哪些引脚真的在跳。

已验证：往 console 狂发字节时，这个寄存器里对应 UART0_TX 的 bit 会跳 → 方法有效。
（偏移 0x00 是输出数据寄存器，0x50 才是引脚真实电平 EXT_PORT）

用法：python3 gpio_scan_pads.py [秒数]
"""
import mmap
import os
import struct
import sys
import time

EXT_PORT = 0x50
BANKS = [("A", 0x03020000), ("B", 0x03021000), ("C", 0x03022000), ("D", 0x03023000)]
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
PAGE = 0x1000

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
maps = {}
for name, base in BANKS:
    off = base & ~(PAGE - 1)
    m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)
    maps[name] = (m, base - off + EXT_PORT)

def read(name):
    m, d = maps[name]
    return struct.unpack_from("<I", m, d)[0]

print("初始电平：")
for n, _ in BANKS:
    print("  GPIO%s EXT_PORT = 0x%08X" % (n, read(n)))

prev = {n: read(n) for n, _ in BANKS}
cnt = {n: [0] * 32 for n, _ in BANKS}
samples = 0
t0 = time.time()
while time.time() - t0 < SECONDS:
    for n, _ in BANKS:
        v = read(n)
        d = v ^ prev[n]
        if d:
            for b in range(32):
                if (d >> b) & 1:
                    cnt[n][b] += 1
            prev[n] = v
    samples += 1

el = time.time() - t0
print()
print("采样 %d 轮 / %.2f 秒（%.0f 轮/秒），只读" % (samples, el, samples / el))
print()
any_hit = False
for n, _ in BANKS:
    hits = [(b, c) for b, c in enumerate(cnt[n]) if c > 0]
    if not hits:
        print("  GPIO%s: 静止" % n)
        continue
    any_hit = True
    print("  GPIO%s: 在跳变的引脚" % n)
    for b, c in sorted(hits, key=lambda x: -x[1]):
        print("      GPIO%s%-2d  跳变 %5d 次" % (n, b, c))
print()
if not any_hit:
    print("结论：所有引脚静止 → 电控的信号没有到达 SoC 任何引脚")
os.close(fd)
