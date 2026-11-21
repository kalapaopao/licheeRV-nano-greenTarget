#!/bin/sh
# 只测真串口：跑 6 秒，用 TERM 结束（后台进程会忽略 INT）
set -u
cd /code/fly_LicheeRV || exit 1

for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in
        *uart_listen*) kill -9 "$pid" 2>/dev/null ;;
        *fly_LicheeRV/camera*) kill "$pid" 2>/dev/null ;;
        *fake_mcu_pty*) kill "$pid" 2>/dev/null ;;
    esac
done
sleep 2

echo "=== 内核计数（基线）==="
grep -E '^1: ' /proc/tty/driver/serial

python3 tools/board/uart_listen.py /dev/ttyS1 > /tmp/lis_real.log 2>&1 &
P=$!
sleep 6
kill -TERM $P 2>/dev/null
sleep 1
kill -9 $P 2>/dev/null
wait $P 2>/dev/null

echo
echo "=== 监听输出（真 /dev/ttyS1，6 秒）==="
cat /tmp/lis_real.log

echo
echo "=== 内核计数（之后）==="
grep -E '^1: ' /proc/tty/driver/serial
