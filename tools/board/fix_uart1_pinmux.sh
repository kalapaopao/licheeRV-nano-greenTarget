#!/bin/sh
# 按 Sipeed 官方 wiki 配置 UART1 引脚复用
# 默认：A28=UART1 TX, A29=UART1 RX, A18=UART1 CTS, A19=UART1 RTS
# 若要把 UART1 的数据脚改到 A19(TX)/A18(RX)（我们现在接线的地方）：
#   devmem 0x03001068 32 0x6   # GPIOA18 → UART1 RX
#   devmem 0x03001064 32 0x6   # GPIOA19 → UART1 TX
# （此时 A28/A29 变成 UART2，我们用不到 UART2，不影响）
set -u

echo "=== 改之前 ==="
echo "  A18(0x03001068) = $(devmem 0x03001068 32)   (4=UART1 CTS)"
echo "  A19(0x03001064) = $(devmem 0x03001064 32)   (4=UART1 RTS)"
echo "  A28(0x03001070) = $(devmem 0x03001070 32)   (1=UART1 TX)"
echo "  A29(0x03001074) = $(devmem 0x03001074 32)   (1=UART1 RX)"

echo
echo "=== 把 UART1 的数据脚改到 A19/A18（对应我们现在的接线）==="
devmem 0x03001068 32 0x6 && echo "  A18 → UART1 RX  写入 0x6 完成"
devmem 0x03001064 32 0x6 && echo "  A19 → UART1 TX  写入 0x6 完成"

echo
echo "=== 改之后 ==="
echo "  A18 = $(devmem 0x03001068 32)"
echo "  A19 = $(devmem 0x03001064 32)"

sleep 1
echo
echo "=== 内核串口计数（rx 有没有开始涨）==="
grep -E '^1: ' /proc/tty/driver/serial
