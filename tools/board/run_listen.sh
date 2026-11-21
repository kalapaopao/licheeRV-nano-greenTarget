#!/bin/sh
# 跑修正后的监听器 10 秒：停相机 -> uart_listen -> 结束后报内核计数
set -u
cd /code/fly_LicheeRV || exit 1

sh stop.sh >/dev/null 2>&1 || true
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    if grep -q 'fly_LicheeRV/camera' "$p/cmdline" 2>/dev/null; then kill "$pid" 2>/dev/null; fi
done
sleep 2

echo "基线: $(grep -E '^1: ' /proc/tty/driver/serial)"
python3 tools/board/uart_listen.py /dev/ttyS1 &
P=$!
sleep 10
kill -TERM $P 2>/dev/null
sleep 1
kill -9 $P 2>/dev/null
wait $P 2>/dev/null
echo
echo "之后: $(grep -E '^1: ' /proc/tty/driver/serial)"
