#!/usr/bin/env python3
"""试验：把 GPIOA18 的引脚复用值逐个试一遍，看哪个值能让 UART1 收到数据。

背景（已被本脚本的前置实验证明）：
  - GPIOA18 的焊盘上确实有电控的数据（EXT_PORT 寄存器里 A18 每秒跳变 600+ 次）
  - 但 ttyS1 的内核 rx 一直是 0 → 说明 A18 没被复用成 UART1_RX
  - pinmux 寄存器块在 0x03001000，疑似一个引脚一个字（低位是功能号）
  - A19 的值是 3（可能已复用成 UART1_TX），A18 的值是 0

风险与安全措施：
  - 这些是易失寄存器，断电/重启自动恢复默认
  - 只动 0x03001048 这一个字（疑似 A18）
  - 每个候选值只保留 2 秒，测完立刻恢复原值
  - 不碰其它任何寄存器
"""
import mmap
import os
import struct
import sys
import time

FMUX_BASE = 0x03001000
REG_OFF = 0x48            # 0x03001048，疑似 GPIOA18
GPIO_BASE = 0x03020000
EXT_PORT = 0x50
PAGE = 0x1000

def port_rx(dev_num):
    try:
        with open("/proc/tty/driver/serial") as f:
            for line in f:
                if line.startswith("%d:" % dev_num):
                    for tok in line.split():
                        if tok.startswith("rx:"):
                            return int(tok[3:])
    except OSError:
        pass
    return -1

def a18_levels(mm_gpio):
    v = struct.unpack_from("<I", mm_gpio, EXT_PORT)[0]
    return (v >> 18) & 1

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
mf = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
               offset=FMUX_BASE)
mg = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ,
               offset=GPIO_BASE)

orig = struct.unpack_from("<I", mf, REG_OFF)[0]
print("0x03001048 当前值 = 0x%08X（准备逐个试 0..7，测完恢复）" % orig)
print()

# 先确认 A18 上还有信号（电控还在发）
flips = 0
prev = a18_levels(mg)
t0 = time.time()
while time.time() - t0 < 1.0:
    cur = a18_levels(mg)
    if cur != prev:
        flips += 1
        prev = cur
print("前置检查：A18 引脚 1 秒内跳变 %d 次%s" %
      (flips, "  ✓ 电控在发" if flips > 20 else "  ⚠ 信号很少，可能电控没发"))
print()

best = None
try:
    for val in range(8):
        struct.pack_into("<I", mf, REG_OFF, val)      # 写引脚复用
        time.sleep(0.2)
        before = port_rx(1)
        time.sleep(1.8)
        after = port_rx(1)
        delta = after - before
        flag = ""
        if delta > 0:
            flag = "   ←←← 收到数据了！"
            if best is None:
                best = val
        print("  复用值 %d → ttyS1 rx %d → %d（+%d）%s"
              % (val, before, after, delta, flag))
finally:
    struct.pack_into("<I", mf, REG_OFF, orig)          # 无论如何都恢复
    print()
    print("已恢复 0x03001048 = 0x%08X" % orig)

print()
if best is not None:
    print("结论：复用值 %d 能收到数据 → A18 的 UART1_RX 功能号是 %d" % (best, best))
    print("      需要把这个值固化（写进启动脚本或设备树）")
else:
    print("结论：0..7 都收不到 → 0x03001048 不是 A18 的寄存器，索引映射不是 pin*4")
    print("      下一步需要 SG2002 的 pinmux 表，或用 u-boot 侧配置")
os.close(fd)
