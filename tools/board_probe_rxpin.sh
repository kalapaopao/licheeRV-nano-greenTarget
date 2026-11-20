#!/bin/sh
# 把 GPIOA18（UART1_RX 那根）当普通输入读一下：
#   电平在翻转 → 线上有信号，问题在「引脚没被复用成 UART1_RX」
#   电平不动   → 线上没有信号（接线/电控侧问题）
set -u
GPIO=370          # gpiochip352 = bank A，GPIOA18 => 352 + 18
BASE=/sys/class/gpio

if [ ! -d "$BASE/gpio$GPIO" ]; then
    echo $GPIO > "$BASE/export" 2>/dev/null || echo "(export 失败)"
    sleep 0.3
fi
echo in > "$BASE/gpio$GPIO/direction" 2>/dev/null || echo "(设成输入失败)"
sleep 0.2

echo "=== GPIOA18 电平采样（每 30ms 一次，共 40 次 ≈ 1.2 秒）==="
i=0
ones=0
zeros=0
flips=0
prev=""
while [ $i -lt 40 ]; do
    V=$(cat "$BASE/gpio$GPIO/value" 2>/dev/null || echo "?")
    printf '%s' "$V"
    [ "$V" = "1" ] && ones=$((ones + 1))
    [ "$V" = "0" ] && zeros=$((zeros + 1))
    if [ -n "$prev" ] && [ "$V" != "$prev" ]; then flips=$((flips + 1)); fi
    prev=$V
    i=$((i + 1))
    sleep 0.03
done
echo
echo "高电平 $ones 次 / 低电平 $zeros 次 / 翻转 $flips 次"

if [ "$flips" -gt 0 ]; then
    echo "=> 线上有跳变：信号到板子了，问题是这跟引脚没被复用成 UART1_RX"
else
    echo "=> 电平不动：这根线上没有信号（接线/共地/电控侧问题）"
fi

# 收尾：别把引脚留在 GPIO 状态
echo $GPIO > "$BASE/unexport" 2>/dev/null || true
