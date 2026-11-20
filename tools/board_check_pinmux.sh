#!/bin/sh
# 看 UART1 的引脚有没有被复用成串口功能（TX 能发说明至少 TX 通了，重点看 RX）
P=/sys/kernel/debug/pinctrl
echo "=== pinctrl 控制器 ==="
ls "$P" 2>/dev/null | head -5
for d in "$P"/*/; do
    echo "[$d]"
    grep -iE 'uart|a18|a19' "$d/pinmux-pins" 2>/dev/null | head -8
done
echo
echo "=== 串口设备节点 ==="
ls -l /dev/ttyS* 2>/dev/null
echo
echo "=== 内核 dmesg 里的 uart/ttyS（最后 10 行相关）==="
dmesg 2>/dev/null | grep -iE 'ttyS|uart|16550' | tail -10
