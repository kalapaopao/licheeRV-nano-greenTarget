#!/bin/sh
# 看收/发帧数比值是否恒定（判断电控真实速率 vs 重复计数）
LOG=/tmp/fly_licheerv_capture.log
echo "=== 所有 UART 统计行 ==="
grep -E '^UART sent=' "$LOG" 2>/dev/null | tail -12
echo
echo "=== 逐行算 rx/sent 比值 ==="
grep -E '^UART sent=' "$LOG" 2>/dev/null | tail -8 | while read -r line; do
    S=$(echo "$line" | sed -n 's/.*sent=\([0-9]*\).*/\1/p')
    R=$(echo "$line" | sed -n 's/.*rx_frames=\([0-9]*\).*/\1/p')
    if [ -n "$S" ] && [ -n "$R" ] && [ "$S" -gt 0 ]; then
        awk -v s="$S" -v r="$R" 'BEGIN{printf "  sent=%-6d rx=%-7d rx/sent=%.3f  推算rx速率=%.0f Hz\n", s, r, r/s, (r/s)*50}'
    fi
done
echo
echo "说明: 协议写的是电控 100Hz。若比值恒定在 ~6.7，说明电控实际发 ~333Hz；"
echo "      rx_crc=0 说明解析没问题，但 333Hz×33B = 11KB/s，已接近 115200 上限(11.5KB/s)。"
