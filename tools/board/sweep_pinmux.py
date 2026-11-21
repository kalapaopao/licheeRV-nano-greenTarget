#!/usr/bin/env python3
"""穷举 pinmux 块，找出哪个寄存器 + 哪个值能让 UART1 收到数据。

已知（本会话已实测）：
  - GPIOA18 焊盘上有电控数据（EXT_PORT bit18 每秒跳变数百次）
  - 但 ttyS1 的 rx 恒为 0 → A18 没被复用成 UART1_RX
  - 0x03001000 附近有一个"一个引脚一个字、低位是功能号"的块（无驱动占用）
  - 直接按 pin*4 猜 A18=0x48 试 0..7 都没成功 → 索引映射不是 pin*4

本脚本：把这一整块（0x03001000~0x03001200）逐字 + 逐值试一遍，
每次只改低 3 位、80ms 后检查 ttyS1 rx 是否增长，然后立刻恢复原值。
全部寄存器是易失的，断电即恢复；结束时会整体还原。
"""
import mmap
import os
import struct
import sys
import time

BASE = 0x03001000
SPAN = 0x200              # 128 个 32 位寄存器
PAGE = 0x1000
WINDOW = 0.09             # 每个候选保留 90ms（100Hz × 33B = 3.3KB/s，足够看出 rx 增长）

def rx1():
    try:
        with open("/proc/tty/driver/serial") as f:
            for line in f:
                if line.startswith("1:"):
                    for tok in line.split():
                        if tok.startswith("rx:"):
                            return int(tok[3:])
    except OSError:
        pass
    return -1

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
              offset=BASE)

orig = {}
for off in range(0, SPAN, 4):
    orig[off] = struct.unpack_from("<I", m, off)[0]

print("已备份 %d 个寄存器。开始穷举（每值 90ms）..." % len(orig))
print("基线 ttyS1 rx = %d" % rx1())
sys.stdout.flush()

hits = []
t_start = time.time()
try:
    for off in range(0, SPAN, 4):
        o = orig[off]
        for val in range(8):
            if (o & 0x7) == val:
                continue                       # 当前已经是这个值，跳过
            struct.pack_into("<I", m, off, (o & ~0x7) | val)
            time.sleep(WINDOW)
            cur = rx1()
            struct.pack_into("<I", m, off, o)  # 立刻恢复
            if cur > 0:
                hits.append((off, o, val, cur))
                print("  ★ 命中！寄存器 0x%08X 原值 %d → 写成 %d，rx=%d"
                      % (BASE + off, o, val, cur))
                sys.stdout.flush()
                raise SystemExit(0)
        if off % 0x40 == 0:
            print("  ... 已试到 0x%08X（%.0fs）" % (BASE + off, time.time() - t_start))
            sys.stdout.flush()
finally:
    for off, v in orig.items():
        struct.pack_into("<I", m, off, v)
    print()
    print("已还原全部 %d 个寄存器。" % len(orig))

print()
print("耗时 %.0f 秒，命中 %d 次" % (time.time() - t_start, len(hits)))
if not hits:
    print("结论：这一块里没有任何寄存器能把 A18 变成 UART1_RX")
    print("      → pinmux 不在这块，或者 A18 压根不支持 UART1_RX 功能")
os.close(fd)
