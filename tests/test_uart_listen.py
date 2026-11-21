# 协议一致性测试：把文档 §6.3 / §6.4 的样例帧当作测试向量，
# 复用 tools/board/uart_listen.py 的 crc16，验证帧结构/CRC/字段偏移。
import struct
import sys

sys.path.insert(0, ".")

# Windows 上没有 termios/tty/O_NOCTTY（那些只在板端用）。
# 这里只导入 crc16，不执行监听主流程：把 import 塞进 stub 之后再取函数。
import importlib.util
import types

sys.modules.setdefault("termios", types.ModuleType("termios"))
sys.modules.setdefault("tty", types.ModuleType("tty"))
if not hasattr(sys.modules["os"], "O_NOCTTY"):
    sys.modules["os"].O_NOCTTY = 0

spec = importlib.util.spec_from_file_location("uart_listen_crc", "tools/board/uart_listen.py")
mod = importlib.util.module_from_spec(spec)
src = open("tools/board/uart_listen.py", encoding="utf-8").read()
# 只取函数定义部分：截到 `fd = os.open` 之前（主流程从那里开始）
head = src.split("fd = os.open", 1)[0]
exec(compile(head, "uart_listen_head", "exec"), mod.__dict__)
crc16 = mod.crc16

DOC_63 = ("A5 5A 1A 07 02 01 07 00 00 80 3F 00 00 00 C0 00 00 60 40"
          " 00 00 80 40 00 00 A8 C0 00 00 D8 40 73 88")
DOC_64 = ("A5 5A 1A 00 02" + " 00" * 26 + " 1F 38")


def frame_of(hexstr):
    return bytes.fromhex("".join(hexstr.split()))


f63 = frame_of(DOC_63)
f64 = frame_of(DOC_64)
ok = 0
fail = 0


def check(cond, what):
    global ok, fail
    if cond:
        ok += 1
        print("  PASS", what)
    else:
        fail += 1
        print("  FAIL", what)


# §2 整帧长度 = LEN + 7
check(len(f63) == f63[2] + 7 and len(f63) == 33, "整帧 33 字节 = LEN+7")
check(len(f64) == 33, "心跳帧也是 33 字节")

# §4.1 帧头/LEN/CMD
check(f63[0:2] == b"\xA5\x5A" and f63[2] == 0x1A and f63[4] == 0x02,
      "帧头 A5 5A / LEN 0x1A / CMD 0x02")

# §5 CRC：覆盖 [2, -2)，小端
check(crc16(f63[2:-2]) == int.from_bytes(f63[-2:], "little"), "6.3 CRC(0x8873) 校验通过")
check(crc16(f64[2:-2]) == int.from_bytes(f64[-2:], "little"), "6.4 CRC(0x381F) 校验通过")

# §4.1 字段偏移：state@5 flags@6 roll@7 ... gz@27
state, flags, roll, pitch, yaw, gx, gy, gz = struct.unpack_from("<BB6f", f63, 5)
check(state == 1, "state=1 (FLIGHT)")
check(flags == 0x07, "flags=0x07")
check(abs(roll - 1.0) < 1e-6 and abs(pitch + 2.0) < 1e-6 and abs(yaw - 3.5) < 1e-6,
      "roll=1.0 pitch=-2.0 yaw=3.5（含小端与负号）")
check(abs(gx - 4.0) < 1e-6 and abs(gy + 5.25) < 1e-6 and abs(gz - 6.75) < 1e-6,
      "gx=4.0 gy=-5.25 gz=6.75")
s2, fl2, r2, p2, y2, gx2, gy2, gz2 = struct.unpack_from("<BB6f", f64, 5)
check(s2 == 0 and fl2 == 0x00 and (r2, p2, y2, gx2, gy2, gz2) == (0,) * 6,
      "6.4 心跳：state=0 flags=0 全零")

# §5 CRC 标准自检
check(crc16(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE 自检 0x29B1")

print()
print("=== %d passed, %d failed ===" % (ok, fail))
sys.exit(0 if fail == 0 else 1)
