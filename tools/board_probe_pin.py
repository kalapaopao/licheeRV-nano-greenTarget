#!/usr/bin/env python3
"""高速采样 GPIOA18 电平：判断 UART1_RX 那根线上到底有没有数据。

sysfs 的 value 文件一次 read 就 EOF，所以用「seek(0)+read(1)」循环做连续采样。
采样率约 2~5 万次/秒，足以抓到 115200 baud（一个比特 8.7µs、一帧 33 字节约 2.9ms）。

用法（板端）：python3 /tmp/probe_pin.py [秒数] [gpio号]
"""
import sys
import time

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 2.0
GPIO = sys.argv[2] if len(sys.argv) > 2 else "370"     # GPIOA18 = 352 + 18
PATH = "/sys/class/gpio/gpio%s/value" % GPIO

f = open(PATH, "rb", buffering=0)
ones = zeros = other = 0
flips = 0
prev = None
t0 = time.time()
while time.time() - t0 < SECONDS:
    f.seek(0)
    v = f.read(1)
    if v == b"1":
        ones += 1
        cur = 1
    elif v == b"0":
        zeros += 1
        cur = 0
    else:
        other += 1
        continue
    if prev is not None and cur != prev:
        flips += 1
    prev = cur

total = ones + zeros + other
elapsed = time.time() - t0
print("采样 %.2f 秒，共 %d 次（约 %.0f 次/秒）" % (elapsed, total, total / elapsed))
print("  高电平 %d   低电平 %d   其他 %d   翻转 %d" % (ones, zeros, other, flips))
if flips > 0:
    print("=> 线上有跳变：信号到板子了 → 引脚没被复用成 UART1_RX（需要在 pinctrl/寄存器里配）")
else:
    print("=> 全程不变：这根线上没有数据 → 接线/共地/电控侧问题")
