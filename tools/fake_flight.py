#!/usr/bin/env python3
"""假电控：模拟一次真实飞行（静止 → 起飞 → 飞行 N 秒 → 落地）。

与 fake_mcu_pty.py 的区别：这里周期可精确指定，用来复现「15 秒飞行」。
用法: fake_flight.py <静止秒> <飞行秒> [停止秒]
输出第一行: PTY /dev/pts/N
"""
import os
import struct
import sys
import time
import tty

STATIC_S = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
FLIGHT_S = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
STOP_S = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
HZ = 100.0


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(seq, state, flags, roll, pitch, yaw, gx, gy, gz):
    payload = struct.pack("<BBffffff", state, flags, roll, pitch, yaw, gx, gy, gz)
    body = bytes([0x1A, seq & 0xFF, 0x02]) + payload
    c = crc16(body)
    return b"\xA5\x5A" + body + struct.pack("<H", c)


master, slave = os.openpty()
tty.setraw(slave)
print("PTY", os.ttyname(slave), flush=True)

total = STATIC_S + FLIGHT_S + STOP_S
seq = 0
t0 = time.time()
while True:
    el = time.time() - t0
    if el > total:
        break
    if el < STATIC_S:
        state = 0
    elif el < STATIC_S + FLIGHT_S:
        state = 1
    else:
        state = 2
    # 飞行中给点真实姿态变化
    k = max(0.0, el - STATIC_S)
    roll = 5.0 + (k * 12.0 if state == 1 else 0.0)
    pitch = -8.0 + (k * 3.0 if state == 1 else 0.0)
    yaw = -30.0 + (k * 8.0 if state == 1 else 0.0)
    gx = 30.0 if state == 1 else 0.2
    gy = -20.0 if state == 1 else 0.1
    gz = 15.0 if state == 1 else 0.05
    try:
        os.write(master, frame(seq, state, 0x07, roll, pitch, yaw, gx, gy, gz))
    except OSError:
        break
    seq += 1
    time.sleep(1.0 / HZ)

print("DONE frames=%d state_final=%d" % (seq, state), flush=True)
