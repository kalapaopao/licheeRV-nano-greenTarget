#!/bin/sh
# 关键：uart 的 rx 计数只有在端口被打开、RX 中断使能后才会累加。
# 所以必须一边跑监听程序，一边看它有没有解析出帧。
set -u
cd /code/fly_LicheeRV || exit 1

echo "=== pinmux ==="
echo "  A18 = $(devmem 0x03001068 32)  (6=UART1 RX)"
echo "  A19 = $(devmem 0x03001064 32)  (6=UART1 TX)"

echo
echo "=== 打开 /dev/ttyS1 监听 8 秒（uart_listen.py）==="
python3 tools/board/uart_listen.py /dev/ttyS1 > /tmp/listen_fix.log 2>&1 &
P=$!
sleep 8
kill -TERM $P 2>/dev/null
sleep 1
kill -9 $P 2>/dev/null
wait $P 2>/dev/null

echo "--- 监听输出 ---"
head -20 /tmp/listen_fix.log

echo
echo "=== 监听期间的内核 rx ==="
grep -E '^1: ' /proc/tty/driver/serial
