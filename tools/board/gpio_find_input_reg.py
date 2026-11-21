#!/usr/bin/env python3
"""找出 GPIOA 块里哪个偏移读出来是「引脚真实电平」。

做法：往 console 狂发字节（驱动 UART0_TX 那根引脚），
对 GPIOA 块 0x00~0x7C 每个偏移各采样 300 次，看哪个偏移的值在跳。
那个偏移就是"外部引脚电平寄存器"。
"""
import mmap
import os
import struct
import subprocess
import sys
import threading
import time

BASE = 0x03020000
PAGE = 0x1000
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASE & ~(PAGE - 1))
delta = BASE - (BASE & ~(PAGE - 1))

stop = threading.Event()

def spam():
    while not stop.is_set():
        try:
            with open("/dev/ttyS0", "wb", buffering=0) as f:
                f.write(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" * 4)
        except OSError:
            time.sleep(0.01)

t = threading.Thread(target=spam, daemon=True)
t.start()

# 每个偏移采样，统计变化次数
offsets = list(range(0x00, 0x80, 4))
prev = {}
cnt = {o: 0 for o in offsets}
samples = 0
t0 = time.time()
while time.time() - t0 < 3.0:
    for o in offsets:
        v = struct.unpack_from("<I", m, delta + o)[0]
        p = prev.get(o)
        if p is not None and v != p:
            cnt[o] += 1
        prev[o] = v
    samples += 1

stop.set()
t.join(timeout=1)

print("采样 %d 轮 / 3 秒（console 同时在狂发）" % samples)
print()
print("偏移  值          变化次数")
for o in offsets:
    v = struct.unpack_from("<I", m, delta + o)[0] if False else prev[o]
    mark = "  ←←← 引脚电平在这里" if cnt[o] > samples * 0.05 else ""
    print("  0x%02X  0x%08X  %6d%s" % (o, v, cnt[o], mark))
os.close(fd)
