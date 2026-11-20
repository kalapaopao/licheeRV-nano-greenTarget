#!/usr/bin/env python3
"""板上联调用的假电控：建一个 pty，按 100Hz 发 CMD=0x02 帧，STATE 按脚本变化。

用途：没有真实电控时，验证「视觉侧收到 FLIGHT 边沿 → 预录落盘 → 落地收尾」整条链路。
把 pty 从设备路径写到 /tmp/mcu_pty_path，供 UART_DEVICE 使用。
"""
import os
import pty
import struct
import sys
import time


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(seq: int, state: int, flags: int, roll: float, pitch: float, yaw: float,
          gx: float, gy: float, gz: float) -> bytes:
    payload = struct.pack("<BB6f", state, flags, roll, pitch, yaw, gx, gy, gz)
    body = bytes((len(payload), seq & 0xFF, 0x02)) + payload
    return b"\xA5\x5A" + body + struct.pack("<H", crc16_ccitt_false(body))


def main() -> int:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    with open("/tmp/mcu_pty_path", "w") as f:
        f.write(path)
    print("PTY %s" % path, flush=True)

    seq = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        el = time.time() - t0
        # 15s 静止（等相机起来）→ 6s 飞行 → 4s 撞击后停止 → 回到静止
        if el < 15.0:
            state = 0
        elif el < 21.0:
            state = 1
        elif el < 25.0:
            state = 2
        else:
            state = 0
        t = el
        f = frame(seq, state, 0x07, 1.0 + t, -2.0, 3.5, 4.0, -5.25, 6.75)
        try:
            os.write(master, f)
        except OSError:
            break
        seq += 1
        time.sleep(0.01)
    print("DONE frames=%d" % seq, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
