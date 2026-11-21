#!/usr/bin/env python3
"""把电控发来的帧打印到终端 —— 就这一个功能，不做别的。

用法（板端）：
    sh stop.sh                                  # 相机必须停，否则字节被它读走
    python3 tools/board/uart_listen.py          # 默认 /dev/ttyS1
    python3 tools/board/uart_listen.py /dev/ttyS2
    Ctrl-C 退出

打印的样子：
    [   1] seq=  7 FLIGHT flags=0x07(IMU ok) roll=   1.00 pitch=  -2.00 yaw=    3.50 | gx=   4.00 gy=  -5.25 gz=   6.75
没数据时每秒打一行：`(还没收到任何字节)`
"""
import os
import struct
import sys
import termios
import time
import tty

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyS1"
STATE = {0: "STATIC", 1: "FLIGHT", 2: "STOP"}


def crc16(data):
    """CRC-16/CCITT-FALSE：poly 0x1021, init 0xFFFF"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)                                  # 8N1、无回显、无行缓冲
attr = termios.tcgetattr(fd)
attr[4] = attr[5] = termios.B115200             # 输入/输出波特率
attr[2] &= ~termios.CRTSCTS                     # 关硬件流控
attr[0] &= ~(termios.IXON | termios.IXOFF)      # 关软件流控
termios.tcsetattr(fd, termios.TCSANOW, attr)
print("监听 %s @115200 8N1（相机要先停：sh stop.sh；Ctrl-C 退出）" % DEV, flush=True)

buf = b""
frames = bytes_seen = bad_crc = 0
last_line = 0.0
last_rx = time.time()

while True:
    try:
        chunk = os.read(fd, 4096)               # 非阻塞：没数据会抛 BlockingIOError
    except BlockingIOError:
        chunk = b""
        if time.time() - last_rx >= 1.0:        # 1 秒没动静，说一声，别让人干等
            print("(还没收到任何字节)", flush=True)
            last_rx = time.time()
        time.sleep(0.02)
        continue

    if chunk:
        last_rx = time.time()
        bytes_seen += len(chunk)
        buf += chunk

    # ---- 按协议 §8 找帧 ----
    # A5 5A | LEN | SEQ | CMD | PAYLOAD(LEN) | CRC_L CRC_H，整帧 = LEN + 7
    # §8.2 先找帧头，攒够 5 字节后核对 CMD 与 LEN，再等 LEN+7 字节
    # §8.4 坏帧从候选帧头之后丢 1 字节继续同步（不能整帧吞掉，否则丢同步）
    # §8.5 float 负载里也可能出现 A5 5A，所以按长度切，不在负载里找帧头
    while True:
        i = buf.find(b"\xA5\x5A")
        if i < 0:
            buf = buf[-1:]                      # §8.2 保留可能的半个帧头
            break
        if i > 0:
            buf = buf[i:]
        if len(buf) < 5:
            break
        length, cmd = buf[2], buf[4]
        if length != 0x1A or cmd != 0x02:       # §8.3 视觉端只收 CMD=0x02/LEN=0x1A
            buf = buf[1:]                       # 组合不对：丢 1 字节重新找帧头
            continue
        total = length + 7
        if len(buf) < total:
            break                               # §8.4 残帧保留，等后续数据
        f, buf = buf[:total], buf[total:]
        if crc16(f[2:-2]) != int.from_bytes(f[-2:], "little"):
            bad_crc += 1
            buf = b"\xA5" + buf                 # §8.4 坏帧：只丢帧头后 1 字节继续同步
            continue
        frames += 1
        seq = f[3]
        state, flags, roll, pitch, yaw, gx, gy, gz = struct.unpack_from("<BB6f", f, 5)
        if time.time() - last_line >= 0.1:      # 电控 100Hz，终端 10Hz 打就够了
            last_line = time.time()
            print("[%4d] seq=%3d %-6s flags=0x%02X(%s) roll=%7.2f pitch=%6.2f yaw=%7.2f"
                  " | gx=%7.2f gy=%7.2f gz=%7.2f"
                  % (frames, seq, STATE.get(state, "?%d" % state), flags,
                     "IMU ok" if flags & 0x01 else "IMU invalid", roll, pitch, yaw, gx, gy, gz),
                  flush=True)
