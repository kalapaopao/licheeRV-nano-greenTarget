#!/bin/sh
# 改完 pinmux 后验证：连续 8 秒看 ttyS1 的 rx 是否增长
set -u
echo "=== pinmux 当前值 ==="
echo "  A18(0x03001068) = $(devmem 0x03001068 32)   (6=UART1 RX)"
echo "  A19(0x03001064) = $(devmem 0x03001064 32)   (6=UART1 TX)"
echo
echo "=== ttyS1 rx 变化（每秒一次，看 8 秒）==="
i=0
while [ $i -lt 8 ]; do
    LINE=$(grep -E '^1: ' /proc/tty/driver/serial)
    RX=$(echo "$LINE" | sed -n 's/.*rx:\([0-9]*\).*/\1/p')
    TX=$(echo "$LINE" | sed -n 's/.*tx:\([0-9]*\).*/\1/p')
    echo "  t=${i}s  rx=$RX  tx=$TX"
    i=$((i + 1))
    sleep 1
done
