#!/bin/sh
# 四个 UART 一起监听（板子重启过、计数已清零，正好全查一遍）：
# 每个口跑 6 秒 uart_listen，看电控的数据到底落在哪个口上
set -u
cd /code/fly_LicheeRV || exit 1

sh stop.sh >/dev/null 2>&1 || true
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    if grep -q 'fly_LicheeRV/camera' "$p/cmdline" 2>/dev/null; then kill "$pid" 2>/dev/null; fi
done
sleep 2

echo "=== 各口基线计数 ==="
cat /proc/tty/driver/serial | sed 's/^/  /'

for N in 0 1 2 3; do
    DEV=/dev/ttyS$N
    [ -c "$DEV" ] || continue
    echo
    echo "=== 监听 $DEV（6 秒）==="
    python3 tools/board/uart_listen.py "$DEV" > "/tmp/lisS$N.log" 2>&1 &
    P=$!
    sleep 6
    kill -TERM $P 2>/dev/null
    sleep 1
    kill -9 $P 2>/dev/null
    wait $P 2>/dev/null
    head -4 "/tmp/lisS$N.log" | sed 's/^/  /'
    echo "  -> $DEV 计数: $(grep -E "^$N: " /proc/tty/driver/serial)"
done
