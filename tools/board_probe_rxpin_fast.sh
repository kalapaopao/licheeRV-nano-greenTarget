#!/bin/sh
# 高速采样 GPIOA18：dd 连续读 value（不 fork），能抓到 115200 的跳变
set -u
GPIO=370
BASE=/sys/class/gpio

[ -d "$BASE/gpio$GPIO" ] || { echo $GPIO > "$BASE/export"; sleep 0.3; }
echo in > "$BASE/gpio$GPIO/direction"

# 连续读 4000 个字节（= 4000 次采样，含换行），输出 '0'/'1'
dd if="$BASE/gpio$GPIO/value" bs=1 count=4000 2>/dev/null > /tmp/pin.txt || true

ONES=$(tr -cd '1' < /tmp/pin.txt | wc -c)
ZEROS=$(tr -cd '0' < /tmp/pin.txt | wc -c)
TOTAL=$((ONES + ZEROS))
echo "采样 $TOTAL 次：高 $ONES  低 $ZEROS"
if [ "$ZEROS" -gt 0 ]; then
    echo "=> 线上有跳变（抓到 $ZEROS 个低电平）→ 信号到板子了，问题是引脚没被复用成 UART1_RX"
else
    echo "=> 全程高电平 → 这根线上没有数据（接线/共地/电控侧）"
fi
echo $GPIO > "$BASE/unexport" 2>/dev/null || true
