#!/bin/sh
# 一眼看板端现在到底在干什么：进程 / 温度 / 最近日志 / 运行历史 / 最近一次记录
#
#   sh status.sh
#
# 只读，不改任何状态；自启动"到底跑没跑起来"用这个最快。
ROOT=/code/fly_LicheeRV
STATE=${STATE_DIR:-$ROOT/state}
LOG=/tmp/fly_licheerv_capture.log
BOOTLOG=/tmp/fly_boot.log

echo "== 系统 =="
echo "host=$(hostname) uptime=$(cut -d. -f1 /proc/uptime)s load=$(cut -d' ' -f1-3 /proc/loadavg)"
T=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo -1)
echo "temp=$((T / 1000)).$((T % 1000 / 100))C   (74.5C 会触发停机保护)"
CLK=$(devmem 0x030028c4 32 2>/dev/null || echo unknown)
echo "clock_reg=$CLK   (0x00210001 = 36MHz 加速态, 0x00200001 = 原始)"

echo
echo "== 串口链路（内核计数，最权威）=="
DEV=$(grep -hE '^uart_device:' "$ROOT"/config.yaml 2>/dev/null | awk '{print $2}')
echo "设备=$DEV"
# /proc/tty/driver/serial 第 N 行对应 /dev/ttySN；rx 是内核收到的字节数
if [ -n "${DEV:-}" ] && [ -r /proc/tty/driver/serial ]; then
    N=${DEV##*/ttyS}
    LINE=$(grep -E "^$N: " /proc/tty/driver/serial 2>/dev/null)
    if [ -n "$LINE" ]; then
        echo "  内核: $LINE"
        RXK=$(echo "$LINE" | sed -n 's/.*rx:\([0-9]*\).*/\1/p')
        echo "  → 内核收字节数 rx=$RXK"
        if [ "${RXK:-0}" = "0" ]; then
            echo "  ⚠ rx=0：电控的字节根本没有到达这颗 SoC（接线/引脚/电平问题，不是软件问题）"
        fi
    else
        echo "  (找不到 /dev/ttyS$N 的驱动行)"
    fi
fi
if [ -n "$DEV" ] && [ -c "$DEV" ]; then
    L=$(stty -F "$DEV" -a 2>/dev/null | head -1)
    [ -n "$L" ] && echo "  行设置: $L"
fi
# 我们自己的解析计数（从运行日志里抓最后一条 UART 行）
LASTU=$(grep -h 'UART sent=' "$LOG" 2>/dev/null | tail -1)
[ -n "$LASTU" ] && echo "  程序: $LASTU"

echo
echo "== 进程 =="
PID=$(pidof camera 2>/dev/null || true)
if [ -n "$PID" ]; then
    echo "camera 正在运行 pid=$PID"
    ps w | grep -E "^ *$PID " | head -1
    awk '/VmRSS/{print "  "$0}' /proc/$PID/status 2>/dev/null || true
else
    echo "camera 未运行"
fi
SUP=$(ps w | awk '/start[.]sh/ && !/awk/ {print $1; exit}')
[ -n "$SUP" ] && echo "supervisor(start.sh) 在跑 pid=$SUP" || echo "supervisor(start.sh) 不在（相机若还在跑就是孤儿）"

echo
echo "== 最近日志（$LOG）=="
if [ -f "$LOG" ]; then
    tail -8 "$LOG"
else
    echo "(没有 $LOG)"
fi

echo
echo "== 运行历史（$STATE/run_history.log，跨重启保留）=="
if [ -f "$STATE/run_history.log" ]; then
    tail -20 "$STATE/run_history.log"
else
    echo "(没有历史文件：可能还没跑过，或 state_dir 不在默认位置)"
fi

echo
echo "== 最近一次飞行记录 =="
if [ -d "$ROOT/logs" ]; then
    LAST=$(ls -1d "$ROOT/logs"/sess_* 2>/dev/null | tail -1)
    if [ -n "$LAST" ]; then
        echo "$LAST"
        grep -E 'trigger_mono_us|close_reason|rows_frames|rows_mcu|dropped' "$LAST/meta.txt" 2>/dev/null | sed 's/^/  /'
        wc -l "$LAST"/frames.csv "$LAST"/mcu.csv 2>/dev/null | sed 's/^/  /'
    else
        echo "(logs/ 下还没有会话：没触发过，或 log_enable=false)"
    fi
else
    echo "(没有 $ROOT/logs)"
fi

echo
echo "== 配置摘要 =="
grep -hE '^(uart_enable|uart_tx_enable|uart_device|log_enable|log_mode|print_enable|display_enable|record_enable):' "$ROOT"/config.yaml 2>/dev/null | sed 's/^/  config.yaml: /'
