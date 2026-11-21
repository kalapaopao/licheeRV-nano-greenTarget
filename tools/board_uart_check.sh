#!/bin/sh
# 串口链路一键诊断（板端）：把「四个层次」合成一条命令。
#
#   ① 内核计数   —— 每个 UART 收了多少字节（驱动统计，和我们的代码无关）
#   ② 原始抓包   —— 停相机后直接从串口设备抓 3 秒字节并 hexdump
#   ③ 引脚电平   —— 把 UART1_RX 那个焊盘当普通输入高速采样，判「线上有没有跳变」
#   ④ 设备树     —— serial 节点状态、有没有 pinctrl、gpiochip 列表
#
# 用法（板端）：
#   sh /tmp/board_uart_check.sh            # 默认诊断 config.yaml 里的 uart_device
#   sh /tmp/board_uart_check.sh /dev/ttyS2 # 指定设备
#   sh /tmp/board_uart_check.sh -k         # 不停止相机（相机在跑时字节会被它读走，仅看计数）
set -u
ROOT=${FLY_ROOT:-/code/fly_LicheeRV}
KEEP_CAM=0
DEV=""
for a in "$@"; do
    case "$a" in
        -k) KEEP_CAM=1 ;;
        /dev/*) DEV="$a" ;;
    esac
done
[ -n "$DEV" ] || DEV=$(grep -hE '^uart_device:' "$ROOT/config.yaml" 2>/dev/null | awk '{print $2}')
[ -n "$DEV" ] || DEV=/dev/ttyS1
PORT=${DEV##*/ttyS}

rx_of() { grep -E "^$1: " /proc/tty/driver/serial 2>/dev/null | sed -n 's/.*rx:\([0-9]*\).*/\1/p'; }

echo "======== ① 内核串口计数 ========"
cat /proc/tty/driver/serial 2>/dev/null | sed 's/^/  /'
BEFORE=$(rx_of "$PORT")
echo "  → 目标设备 $DEV（ttyS$PORT）当前 rx=${BEFORE:-?}"

if [ "$KEEP_CAM" = "0" ]; then
    sh "$ROOT/stop.sh" >/dev/null 2>&1 || true
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        if grep -q 'fly_LicheeRV/camera' "$p/cmdline" 2>/dev/null; then kill "$pid" 2>/dev/null; fi
    done
    sleep 2
    echo "  （已停止相机，避免字节被它读走）"
else
    echo "  （-k：相机仍在跑，抓包结果可能为空）"
fi

if [ -c "$DEV" ]; then
    stty -F "$DEV" 115200 cs8 -cstopb -parenb raw -echo -crtscts -ixon -ixoff 2>/dev/null
fi

echo
echo "======== ② 原始抓包 3 秒 ========"
rm -f /tmp/uart_raw.bin
if [ -c "$DEV" ]; then
    dd if="$DEV" of=/tmp/uart_raw.bin bs=1 count=600 2>/dev/null &
    DDPID=$!
    sleep 3
    kill $DDPID 2>/dev/null
    wait $DDPID 2>/dev/null
fi
AFTER=$(rx_of "$PORT")
SIZE=$(wc -c < /tmp/uart_raw.bin 2>/dev/null || echo 0)
echo "  内核 rx: ${BEFORE:-?} → ${AFTER:-?}   增量 $(( ${AFTER:-0} - ${BEFORE:-0} )) 字节"
echo "  抓到: $SIZE 字节"
if [ "$SIZE" -gt 0 ]; then
    echo "  前 96 字节:"
    od -An -tx1 -v /tmp/uart_raw.bin | head -6 | sed 's/^/    /'
fi

echo
echo "======== ③ UART1_RX 焊盘电平（GPIOA18 = gpio370）========"
cat > /tmp/_pin_probe.py <<'PYEOF'
import sys, time
path = "/sys/class/gpio/gpio%s/value" % sys.argv[2]
sec = float(sys.argv[1])
f = open(path, "rb", buffering=0)
ones = zeros = flips = 0
prev = None
t0 = time.time()
while time.time() - t0 < sec:
    f.seek(0)
    v = f.read(1)
    if v == b"1":
        ones += 1; cur = 1
    elif v == b"0":
        zeros += 1; cur = 0
    else:
        continue
    if prev is not None and cur != prev:
        flips += 1
    prev = cur
total = ones + zeros
print("  采样 %d 次（%.0f/s）：高 %d 低 %d 翻转 %d"
      % (total, total / sec, ones, zeros, flips))
print("  => " + ("线上有跳变：信号到了，问题在引脚复用（UART1_RX 没配）"
                 if flips else "全程不变：这根线上没有数据（接线/共地/电控侧）"))
PYEOF
GPIO=370
[ -d /sys/class/gpio/gpio$GPIO ] || { echo $GPIO > /sys/class/gpio/export 2>/dev/null; sleep 0.3; }
echo in > /sys/class/gpio/gpio$GPIO/direction 2>/dev/null
if [ -e /sys/class/gpio/gpio$GPIO/value ] && command -v python3 >/dev/null 2>&1; then
    python3 /tmp/_pin_probe.py 3 $GPIO
else
    echo "  （没有 gpio 或 python3，跳过）"
fi
echo $GPIO > /sys/class/gpio/unexport 2>/dev/null || true
rm -f /tmp/_pin_probe.py

echo
echo "======== ④ 设备树与 pinctrl ========"
for d in /proc/device-tree/serial@*; do
    [ -d "$d" ] || continue
    st=$(tr -d '\000' < "$d/status" 2>/dev/null | head -c 12)
    echo "  $(basename "$d"): status=$st"
done
if ls -d /proc/device-tree/pinctrl* >/dev/null 2>&1; then
    echo "  pinctrl 节点: 有"
else
    echo "  pinctrl 节点: 无（引脚复用靠开机默认值）"
fi
echo "  gpiochip: $(ls /sys/class/gpio 2>/dev/null | grep -c gpiochip) 个"

echo
echo "======== 结论 ========"
if [ "${AFTER:-0}" -gt "${BEFORE:-0}" ] || [ "$SIZE" -gt 0 ]; then
    echo "  串口收到了字节 → 物理链路通，问题在解析/波特率/协议"
else
    echo "  串口一个字节都没收到（内核计数未增长、抓包为空）"
    echo "  → 数据没到达 SoC：查 电控TX→GPIOA18、共地、3.3V TTL、引脚复用"
    echo "  → 想区分「板子 RX 坏」还是「电控没发」：把 GPIOA19 与 GPIOA18 短接做自环，"
    echo "     再跑一次本脚本，rx 有增长说明板子 RX 通路正常"
fi
