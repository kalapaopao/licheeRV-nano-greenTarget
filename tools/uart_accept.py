#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""串口验收工具（协议 v2，CMD=0x01）：把「电控收到的字节」实时翻译成终端可读的表格。

联调场景：视觉板 TX 上并一根线接 USB-TTL（或直接读电控的调试口日志），
终端里盯这一屏，就能判断「电控到底收到什么、有没有坏帧、数据新不新鲜」。

三种用法：
  1) 实时看串口（USB-TTL 并线监听，最常用）
       python tools/uart_accept.py --port COM7 --baud 115200 --seconds 30
  2) 回放已保存的抓包（串口助手存成 txt 也行，带时间戳没关系）
       python tools/uart_accept.py --file 抓包.txt --replay-hz 50
  3) 管道喂（type 抓包.txt | python tools/uart_accept.py -）

退出码：0 = 验收通过；1 = 有失败项；2 = 打开串口/参数错误（可用于脚本判断）。

只依赖：标准库 + pyserial（仅 --port 模式需要，py -m pip install pyserial）。
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import time
import unicodedata
from collections import deque

FRAME_LEN = 28
DATA_LEN = 0x15
HEAD = b"\xA5\x5A"
BYTE_TOKEN = re.compile(r"^[0-9A-Fa-f]{2}$")

# 协议 §3/§4 给的范围（超出就说明视觉侧算出离谱值）
YAW_LIMIT = 32.0
PITCH_LIMIT = 18.0
DIST_MIN, DIST_MAX = -1.0, 30.0

CSI = {"reset": "\033[0m", "dim": "\033[2m", "red": "\033[31m", "green": "\033[32m",
       "yellow": "\033[33m", "cyan": "\033[36m", "bold": "\033[1m"}


def enable_ansi():
    """Windows 10+ 控制台默认不开 VT 序列，这里手动打开；失败就退化为无色。"""
    if os.name != "nt":
        return True
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if not k.GetConsoleMode(h, ctypes.byref(mode)):
            return False
        return bool(k.SetConsoleMode(h, mode.value | 0x0004))
    except Exception:
        return False


class Color:
    def __init__(self, on):
        self.on = on

    def __call__(self, text, name):
        if not self.on:
            return text
        return CSI[name] + text + CSI["reset"]


def crc16_ccitt_false(buf):
    crc = 0xFFFF
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def text_to_bytes(text):
    """从混杂文本里抽字节：只认连续 >=8 个「两字符十六进制」token 的片段，避免把时间戳当成数据。"""
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


class FrameSplitter:
    """字节流 → 28 字节帧（独立实现，不复用被验收方的代码）。"""

    def __init__(self):
        self.buf = bytearray()
        self.resync_bytes = 0
        self.crc_errors = 0
        self.len_errors = 0

    def feed(self, chunk):
        self.buf.extend(chunk)
        frames = []
        while True:
            # 对齐帧头
            idx = self.buf.find(HEAD)
            if idx < 0:
                # 没有完整帧头：留最后一个字节（可能是 A5）
                drop = max(0, len(self.buf) - 1)
                self.resync_bytes += drop
                del self.buf[:drop]
                break
            if idx > 0:
                self.resync_bytes += idx
                del self.buf[:idx]
            if len(self.buf) < 3:
                break
            length = self.buf[2]
            if length not in (DATA_LEN, 0x1A):
                self.len_errors += 1
                self.resync_bytes += 1
                del self.buf[:1]
                continue
            need = length + 7
            if len(self.buf) < need:
                break
            frame = bytes(self.buf[:need])
            want = struct.unpack("<H", frame[need - 2:need])[0]
            if crc16_ccitt_false(frame[2:need - 2]) != want:
                self.crc_errors += 1
                self.resync_bytes += 1
                del self.buf[:1]
                continue
            del self.buf[:need]
            frames.append(frame)
        return frames


class Stats:
    def __init__(self):
        self.frames = 0
        self.heartbeats = 0
        self.bad_format = 0
        self.out_of_range = 0
        self.seq_loss = 0
        self.seq_jumps = []
        self.max_gap_ms = 0.0
        self.long_gaps = []          # (时刻, ms)
        self.first_t = None
        self.last_t = None
        self.last_seq = None
        self.last_frame = None       # 最近一帧（dict）
        self.window = deque(maxlen=64)   # 最近 64 帧的到达时刻，用于实时显示瞬时帧率
        self.nonfinite = 0

    def add(self, frame, now):
        seq = frame[3]
        detected = frame[5]
        yaw, pitch, vyaw, vpitch, dist = struct.unpack("<5f", frame[6:26])
        rec = {"seq": seq, "detected": detected, "yaw": yaw, "pitch": pitch,
               "vyaw": vyaw, "vpitch": vpitch, "dist": dist, "t": now}

        self.frames += 1
        if detected == 0:
            self.heartbeats += 1
        if detected not in (0, 1):
            self.bad_format += 1
        for v in (yaw, pitch, vyaw, vpitch, dist):
            if not math.isfinite(v):
                self.nonfinite += 1
                self.bad_format += 1
                break
        else:
            if abs(yaw) > YAW_LIMIT or abs(pitch) > PITCH_LIMIT or not (DIST_MIN <= dist <= DIST_MAX):
                self.out_of_range += 1

        if self.last_seq is not None:
            step = (seq - self.last_seq) & 0xFF
            if step != 1:
                miss = (step - 1) & 0xFF
                self.seq_loss += miss
                if len(self.seq_jumps) < 8:
                    self.seq_jumps.append((self.last_seq, seq, miss))
        self.last_seq = seq
        self.last_frame = rec

        if self.last_t is None:
            self.first_t = now
        else:
            gap = (now - self.last_t) * 1000.0
            if gap > self.max_gap_ms:
                self.max_gap_ms = gap
            if gap > 300.0 and len(self.long_gaps) < 8:
                self.long_gaps.append((now, gap))
        self.last_t = now
        self.window.append(now)

    def rate_hz(self):
        if self.first_t is None or self.last_t is None:
            return 0.0
        span = self.last_t - self.first_t
        if span <= 0:
            return 0.0
        return (self.frames - 1) / span

    def inst_rate(self):
        if len(self.window) < 2:
            return 0.0
        span = self.window[-1] - self.window[0]
        return (len(self.window) - 1) / span if span > 0 else 0.0


def disp_width(text):
    """终端显示宽度：中文等全角字符占 2 列，用它算对齐才不会串行。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in text)


def pad(text, width):
    return text + " " * max(0, width - disp_width(text))


def verdict_rows(st, splitter, expect_rate, rate_tol, live_mode):
    rows = []

    def add(name, ok, detail, level_warn=False):
        rows.append((name, "PASS" if ok else ("WARN" if level_warn else "FAIL"), detail))

    rate = st.rate_hz()
    if live_mode:
        lo, hi = expect_rate * (1 - rate_tol), expect_rate * (1 + rate_tol)
        add("帧率", lo <= rate <= hi, "实测 %.2f Hz（期望 %.0f±%.0f%%）" % (rate, expect_rate, rate_tol * 100))
    else:
        add("帧率", True, "回放模式不测（共 %d 帧）" % st.frames, level_warn=True)

    add("CRC 错误", splitter.crc_errors == 0, "crc_errors=%d, resync_bytes=%d" %
        (splitter.crc_errors, splitter.resync_bytes))
    add("长度错误", splitter.len_errors == 0, "len_errors=%d" % splitter.len_errors)
    if live_mode:
        add("最长断流", st.max_gap_ms < 300.0, "max_gap=%.0f ms（协议：>300ms 视为掉线）" % st.max_gap_ms)
    else:
        add("最长断流", True, "回放模式不测", level_warn=True)

    expected = st.frames + st.seq_loss
    loss_ratio = (st.seq_loss / expected) if expected else 0.0
    add("SEQ 丢帧", loss_ratio <= 0.01, "丢 %d/%d 帧（%.2f%%）" % (st.seq_loss, expected, loss_ratio * 100),
        level_warn=loss_ratio <= 0.05)

    add("字段合法", st.bad_format == 0, "格式错误 %d 帧（detected 非 0/1 或 float 非有限数）" % st.bad_format)
    add("数值范围", st.out_of_range == 0,
        "越界 %d 帧（|yaw|>%.0f° 或 |pitch|>%.0f° 或 dist 不在 [%.0f, %.0f]）"
        % (st.out_of_range, YAW_LIMIT, PITCH_LIMIT, DIST_MIN, DIST_MAX))

    hb_ratio = (st.heartbeats / st.frames * 100.0) if st.frames else 0.0
    rows.append(("心跳比例", "INFO", "%d/%d 帧无目标（%.0f%%）" % (st.heartbeats, st.frames, hb_ratio)))
    return rows


def print_live(st, frame_ok, color):
    f = st.last_frame
    if f is None:
        return
    ts = time.strftime("%H:%M:%S", time.localtime(f["t"]))
    ms = int((f["t"] % 1) * 1000)
    flag = color("有目标", "green") if f["detected"] else color("心跳  ", "dim")
    body = "yaw=%7.2f pitch=%6.2f vyaw=%7.2f vpitch=%7.2f dist=%6.3f" % (
        f["yaw"], f["pitch"], f["vyaw"], f["vpitch"], f["dist"])
    line = "%s.%03d  SEQ=%-3d %s %s  %5.1fHz" % (ts, ms, f["seq"], flag, body, st.inst_rate())
    if not frame_ok:
        line = color(line, "red")
    sys.stdout.write("\r" + line.ljust(118))
    sys.stdout.flush()


def print_report(st, splitter, rows, live_mode, label):
    ok = [r for r in rows if r[1] == "PASS"]
    warn = [r for r in rows if r[1] == "WARN"]
    fail = [r for r in rows if r[1] == "FAIL"]
    print()
    print("=" * 78)
    print("串口验收报告" + ("（%s）" % label if label else ""))
    print("=" * 78)
    print("接收帧数 : %d（其中心跳 %d）" % (st.frames, st.heartbeats))
    if live_mode and st.first_t:
        print("统计时长 : %.2f s" % (st.last_t - st.first_t))
    else:
        print("统计时长 : N/A（回放模式）")
    print("-" * 78)
    for name, level, detail in rows:
        print("  %s %s %s" % (pad(name, 12), pad(level, 6), detail))
    if st.seq_jumps:
        print("-" * 78)
        print("  SEQ 跳变样本（前 %d 条）:" % len(st.seq_jumps))
        for a, b, miss in st.seq_jumps:
            print("    %d → %d（丢 %d 帧）" % (a, b, miss))
    if st.long_gaps:
        print("-" * 78)
        print("  长断流（>300ms，前 %d 条）:" % len(st.long_gaps))
        for _, gap in st.long_gaps:
            print("    %.0f ms" % gap)
    print("-" * 78)
    if st.last_frame:
        f = st.last_frame
        print("  最后一帧: SEQ=%d detected=%d yaw=%.2f pitch=%.2f vyaw=%.2f vpitch=%.2f dist=%.3f"
              % (f["seq"], f["detected"], f["yaw"], f["pitch"], f["vyaw"], f["vpitch"], f["dist"]))
    print("=" * 78)
    if fail:
        print("验收结论：FAIL（%d 项失败，%d 项告警）" % (len(fail), len(warn)))
    elif warn:
        print("验收结论：PASS（有 %d 项告警，需人工确认）" % len(warn))
    else:
        print("验收结论：PASS（%d 项全部通过）" % len(ok))
    print("=" * 78)
    return 0 if not fail else 1


def make_frame(seq, detected, yaw, pitch, vyaw, vpitch, dist):
    """构造一帧（仅供自检/测试使用）。"""
    payload = struct.pack("<B5f", detected, yaw, pitch, vyaw, vpitch, dist)
    body = bytes((DATA_LEN, seq & 0xFF, 0x01)) + payload
    return HEAD + body + struct.pack("<H", crc16_ccitt_false(body))


class _FakeSplitter(object):
    def __init__(self, crc_errors=0, len_errors=0, resync_bytes=0):
        self.crc_errors = crc_errors
        self.len_errors = len_errors
        self.resync_bytes = resync_bytes


def selftest():
    """验收工具自检：用合成数据确认「判定逻辑本身」是对的（不需要硬件）。"""
    print("=== uart_accept.py 自检：判定逻辑 ===")
    fails = 0

    def run_case(name, cases, expect_fail, corrupt=(), expect_rate_ok=True):
        nonlocal fails
        splitter = FrameSplitter()
        st = Stats()
        frames = [make_frame(seq, det, 1.0, 2.0, 0.1, 0.2, 3.0) for seq, det, *_ in cases]
        for idx in corrupt:   # 破坏这些帧的 CRC 字节（模拟线路误码）
            fr = bytearray(frames[idx])
            fr[26] ^= 0xFF
            frames[idx] = bytes(fr)
        # 逐帧喂，并按给定时刻推进，模拟真实到达时间
        for i, fr in enumerate(frames):
            t = cases[i][2] if len(cases[i]) > 2 else (i * 0.02)
            for f in splitter.feed(fr):
                st.add(f, t)
        rows = verdict_rows(st, splitter, 50.0, 0.2, True)
        got_fail = {n for n, lv, _ in rows if lv == "FAIL"}
        ok = (got_fail == set(expect_fail))
        print("  %-6s %s   期望失败项=%s 实际失败项=%s"
              % ("PASS" if ok else "FAIL", name, sorted(expect_fail) or "无", sorted(got_fail) or "无"))
        if not ok:
            fails += 1

    # 1) 正常 50Hz，200 帧 = 4s
    good = [(i & 0xFF, i % 5 == 0, i * 0.02) for i in range(200)]
    run_case("正常 50Hz（200 帧）", good, set())

    # 2) 中间丢了 20 帧（SEQ 跳号，丢帧率 9%）
    lossy = [((i if i < 100 else i + 20) & 0xFF, 1, i * 0.02) for i in range(200)]
    run_case("SEQ 丢 20 帧", lossy, {"SEQ 丢帧"})

    # 3) 中间断流 500ms
    gap = [(i & 0xFF, 1, i * 0.02 + (0.5 if i >= 100 else 0.0)) for i in range(200)]
    run_case("中断流 500ms", gap, {"最长断流"})

    # 4) 一帧字段非法（detected=2）
    bad = [(i & 0xFF, 2 if i == 50 else 1, i * 0.02) for i in range(200)]
    run_case("detected=2 一帧", bad, {"字段合法"})

    # 5) 3 帧 CRC 被破坏
    run_case("3 帧 CRC 错", [(i & 0xFF, 1, i * 0.02) for i in range(200)], {"CRC 错误"},
             corrupt=(10, 50, 120))

    print("自检结果：%s" % ("全部通过" if fails == 0 else "%d 项失败" % fails))
    return 0 if fails == 0 else 1


def load_capture(path):
    """读抓包文件：既支持文本（十六进制/带时间戳的串口助手输出），也支持原始二进制抓包。"""
    data = open(path, "rb").read()
    if not data:
        return b""
    printable = sum(1 for b in data[:4096] if 32 <= b < 127 or b in (9, 10, 13))
    ratio = printable / float(min(len(data), 4096))
    if ratio < 0.95:                     # 大量不可打印字节 → 当成原始二进制
        return data
    raw = text_to_bytes(data.decode("utf-8", "ignore"))
    return raw if raw else data


def open_serial(port, baud):
    try:
        import serial
    except ImportError:
        print("缺少 pyserial：请先执行  python -m pip install pyserial", file=sys.stderr)
        raise
    return serial.Serial(port, baud, timeout=0.05)


def list_ports_hint():
    try:
        import serial.tools.list_ports as lp
        ports = list(lp.comports())
        if ports:
            print("可用串口：", file=sys.stderr)
            for p in ports:
                print("  %-8s %s" % (p.device, p.description), file=sys.stderr)
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser(description="协议 v2 串口验收工具（打印电控收到的数据）")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--port", help="串口，例如 COM7 或 /dev/ttyUSB0")
    src.add_argument("--file", help="从抓包文本回放（可以有时间戳/日志文字）")
    src.add_argument("hexdata", nargs="?", help="十六进制字符串，或 - 表示从标准输入读")
    src.add_argument("--hex", dest="hexopt", help="十六进制字符串（等价于位置参数，便于带空格时使用）")
    ap.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("--seconds", type=float, default=30.0, help="验收时长（秒），0=一直跑到 Ctrl+C")
    ap.add_argument("--expect-rate", type=float, default=50.0, help="标称帧率，默认 50Hz")
    ap.add_argument("--rate-tol", type=float, default=0.2, help="帧率容差，默认 20%%")
    ap.add_argument("--replay-hz", type=float, default=0.0, help="回放速度：按该帧率喂数据（0=尽快）")
    ap.add_argument("--quiet", action="store_true", help="只打报告，不打实时行")
    ap.add_argument("--no-color", action="store_true", help="关闭颜色")
    ap.add_argument("--json", dest="json_path", help="把结论写成 JSON（便于存档/对比）")
    ap.add_argument("--label", help="本次验收标签，例如 靶子1米")
    ap.add_argument("--selftest", action="store_true", help="自检本工具的判定逻辑（不需要硬件）")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    use_color = (not args.no_color) and sys.stdout.isatty() and enable_ansi()
    color = Color(use_color)

    splitter = FrameSplitter()
    st = Stats()
    deadline = (time.monotonic() + args.seconds) if args.seconds and args.seconds > 0 else None
    live = (not args.quiet) and sys.stdout.isatty()

    def handle_bytes(chunk, source_is_live):
        for frame in splitter.feed(chunk):
            st.add(frame, time.monotonic())
            if live:
                print_live(st, True, color)
        if live:
            return

    exit_code = 0
    try:
        if args.port:
            ser = open_serial(args.port, args.baud)
            print("监听 %s @ %d 8N1，%s" % (args.port, args.baud,
                                          "%.1f 秒" % args.seconds if deadline else "Ctrl+C 结束"))
            try:
                while deadline is None or time.monotonic() < deadline:
                    data = ser.read(4096)
                    if data:
                        handle_bytes(data, True)
            except KeyboardInterrupt:
                pass
            finally:
                ser.close()
            live_mode = True
        else:
            if args.file:
                raw = load_capture(args.file)
            elif args.hexopt:
                raw = text_to_bytes(args.hexopt)
            elif args.hexdata and args.hexdata != "-":
                raw = text_to_bytes(args.hexdata)
            else:
                raw = text_to_bytes(sys.stdin.read())
            print("回放 %d 字节%s" % (len(raw), "，按 %.0fHz 喂" % args.replay_hz if args.replay_hz else ""))
            if args.replay_hz > 0:
                step = 1.0 / args.replay_hz
                # 每帧 28 字节，按帧切分后逐帧喂，模拟真实到达节奏
                for i in range(0, len(raw), FRAME_LEN):
                    handle_bytes(raw[i:i + FRAME_LEN], False)
                    time.sleep(step)
            else:
                handle_bytes(raw, False)
            live_mode = args.replay_hz > 0   # 回放时若按真实节奏喂，就当作"实时"来算帧率/断流
    except KeyboardInterrupt:
        pass
    except Exception as exc:  # 串口打不开之类
        print("错误：%s" % exc, file=sys.stderr)
        list_ports_hint()
        return 2

    if st.frames == 0:
        print("\n没有收到任何合法帧：检查波特率(115200)、TX/RX 交叉、共地、以及视觉端 uart_format 是否为 binary。")
        print("（调试提示：本工具要求 A5 5A 开头的二进制帧；若收到 $GT,... 文本，说明还在发 ASCII v1）")
        return 1

    rows = verdict_rows(st, splitter, args.expect_rate, args.rate_tol, live_mode)
    exit_code = print_report(st, splitter, rows, live_mode, args.label)

    if args.json_path:
        payload = {
            "label": args.label,
            "frames": st.frames,
            "heartbeats": st.heartbeats,
            "rate_hz": round(st.rate_hz(), 3) if live_mode else None,
            "max_gap_ms": round(st.max_gap_ms, 1),
            "seq_loss": st.seq_loss,
            "crc_errors": splitter.crc_errors,
            "len_errors": splitter.len_errors,
            "resync_bytes": splitter.resync_bytes,
            "bad_format": st.bad_format,
            "out_of_range": st.out_of_range,
            "rows": [{"name": n, "level": l, "detail": d} for n, l, d in rows],
            "pass": exit_code == 0,
        }
        with open(args.json_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
        print("已写入 %s" % args.json_path)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
