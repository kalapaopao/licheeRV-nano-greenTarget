#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把抓到的串口十六进制粘进来，翻译成人话（协议 v2，28 字节二进制帧）。

为什么有这个工具：
  串口助手只会显示 "A5 5A 15 85 01 00 00 ..." 这种十六进制，肉眼没法判断
  「这是几帧」「有没有坏帧」「角度是多少」。这个脚本做三件事：
    1. 按帧头 A5 5A 把字节流切开，告诉你里面到底有几个整帧；
    2. 逐帧重算 CRC16 并和帧尾比对，坏了会标 BAD（不是靠猜）；
    3. 把 21 字节数据区解成 detected / yaw / pitch / vyaw / vpitch / dist。
  协议定义见 docs/UART_PROTOCOL_V2_DESIGN.md。

用法（Windows 命令行，在仓库根目录执行）：
  python tools/decode_v2.py --file 抓包.txt
  type 抓包.txt | python tools/decode_v2.py -
  python tools/decode_v2.py --hex "A5 5A 15 85 01 00 ... 08 EF"

--file / 管道模式会忽略日志文字：只认「连续 8 个以上、每段正好 2 个十六进制字符」的字节串，
所以把串口助手的整段输出（带时间戳、带 RECV HEX/112 <<<）直接存成 txt 粘进来就行。
脚本只依赖 Python 标准库。
"""
import argparse
import re
import struct
import sys

FRAME_LEN = 28        # v2 帧总长
HEAD = b"\xA5\x5A"    # 帧头（就靠它找到一帧的起点）
DATA_LEN = 0x15       # 数据区长度，固定 21
OFF_SEQ, OFF_CMD, OFF_DATA, OFF_CRC = 3, 4, 5, 26

BYTE_TOKEN = re.compile(r"^[0-9A-Fa-f]{2}$")


def crc16_ccitt_false(buf):
    """CRC16-CCITT-FALSE：poly 0x1021 / init 0xFFFF / 不反射（和固件里同一套算法）。"""
    crc = 0xFFFF
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def text_to_bytes(text):
    """从混杂文本里抽出字节序列：只认连续 >=8 个「两字符十六进制」token 的片段。

    这样 "RECV HEX/112 <<<"、"15:59:40.099" 这类日志文字不会被当成数据。
    """
    tokens = text.replace(",", " ").split()
    out, run = [], []
    for tok in tokens:
        if BYTE_TOKEN.match(tok):
            run.append(tok)
            continue
        if len(run) >= 8:
            out.extend(run)
        run = []
    if len(run) >= 8:
        out.extend(run)
    return bytes(int(t, 16) for t in out)


def split_frames(raw):
    """按帧头切帧，返回 ([(帧, crc是否正确)], 跳过字节数, 尾部剩余字节数)。"""
    frames, skipped, pos = [], 0, 0
    while pos + FRAME_LEN <= len(raw):
        if raw[pos:pos + 2] != HEAD:
            pos += 1
            skipped += 1
            continue
        cand = raw[pos:pos + FRAME_LEN]
        if cand[2] != DATA_LEN:          # 长度不对 → 不是帧头，继续往后找
            pos += 1
            skipped += 1
            continue
        crc_calc = crc16_ccitt_false(cand[2:OFF_CRC])
        crc_wire = struct.unpack("<H", cand[OFF_CRC:])[0]
        frames.append((cand, crc_calc == crc_wire))
        pos += FRAME_LEN
    return frames, skipped, len(raw) - pos


def main():
    ap = argparse.ArgumentParser(description="解码协议 v2 的 28 字节二进制帧")
    ap.add_argument("hexdata", nargs="?", help='十六进制字符串，或 "-" 表示从标准输入读')
    ap.add_argument("--file", help="从文本文件读（可以含时间戳等日志文字）")
    ap.add_argument("--hex", dest="hexopt", help="十六进制字符串（比位置参数更稳，不会空格拆参）")
    args = ap.parse_args()

    if args.file:
        text = open(args.file, "r", encoding="utf-8", errors="ignore").read()
    elif args.hexopt:
        text = args.hexopt
    elif args.hexdata == "-" or (args.hexdata is None and not sys.stdin.isatty()):
        text = sys.stdin.read()
    elif args.hexdata:
        text = args.hexdata
    else:
        ap.print_help()
        return 2

    raw = text_to_bytes(text)
    frames, skipped, tail = split_frames(raw)
    print("抽出 %d 字节 = %d 个完整帧（每帧 28 字节）；跳过噪音 %d 字节，尾部不完整 %d 字节"
          % (len(raw), len(frames), skipped, tail))
    if not frames:
        print("没有找到合法帧：检查是不是 A5 5A 开头、波特率 115200、接线 TX/RX 交叉且共地。")
        return 1

    print()
    print(" #   SEQ   CRC(线上/重算)     detected  yaw      pitch    vyaw     vpitch   dist")
    print("--- ---- ------------------ --------- -------- -------- -------- -------- --------")
    prev_seq = None
    lost = bad = heartbeats = 0
    for i, (f, crc_ok) in enumerate(frames, 1):
        seq = f[OFF_SEQ]
        crc_wire = struct.unpack("<H", f[OFF_CRC:])[0]
        crc_calc = crc16_ccitt_false(f[2:OFF_CRC])
        det, yaw, pitch, vyaw, vpitch, dist = struct.unpack("<B5f", f[OFF_DATA:OFF_CRC])
        print("%3d  %3d   0x%04X/0x%04X %-4s %9d %8.2f %8.2f %8.2f %8.2f %8.3f"
              % (i, seq, crc_wire, crc_calc, "OK" if crc_ok else "BAD",
                 det, yaw, pitch, vyaw, vpitch, dist))
        if not crc_ok:
            bad += 1
        if det == 0:
            heartbeats += 1
        if prev_seq is not None:
            step = (seq - prev_seq) & 0xFF
            if step != 1:
                print("      ↑ SEQ %d → %d（中间丢 %d 帧或线路插入了别的内容）"
                      % (prev_seq, seq, (step - 1) & 0xFF))
                lost += (step - 1) & 0xFF
        prev_seq = seq

    print()
    print("小结：%d 帧，%d 帧 CRC 错，%d 帧是「没目标」心跳（detected=0、角度全 0、dist=-1.0）"
          % (len(frames), bad, heartbeats))
    if heartbeats == len(frames):
        print("      全部是心跳 → 视觉此刻没锁定目标（不是串口坏了）")
    if bad:
        print("      ⚠️ 有 CRC 错帧：线上有干扰或波特率不一致，检查线和地")
    elif lost:
        print("      CRC 全对但 SEQ 有断档：可能是对端读取跟不上，或中间被丢掉一段")
    else:
        print("      CRC 全对、SEQ 连续 → 这段链路干净，没有误码/丢帧")
    print("      提醒：v2 没有 age/state 字段，判断数据新旧只能靠「多久没收到帧」。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
