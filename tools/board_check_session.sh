#!/bin/sh
# 看这次「甩动触发 → 落地」到底有没有正确开/关会话
set -u
cd /code/fly_LicheeRV || exit 1

echo "=== record/ ==="
find record -maxdepth 2 -type f 2>/dev/null | sort
echo
echo "=== 每个会话的关键行 ==="
for s in record/sess_*; do
    [ -d "$s" ] || continue
    echo "[$s]"
    ls -l "$s" | awk 'NR>1{printf "   %10s %s\n", $5, $NF}'
    grep -E '^(close_reason|trigger_mono_us|land_mono_us|rows_frames|video_frames|video_last_avi|mcu_stream)' "$s/meta.txt" | sed 's/^/   /'
done
echo
echo "=== 进程还在不在 ==="
ps | grep -v grep | grep -c camera
echo
echo "=== 串口收到的电控帧统计（日志里最后一条 UART 行）==="
grep -h 'UART sent=' /tmp/fly_boot.log /tmp/*.log 2>/dev/null | tail -2
echo
echo "=== 是否有关闭日志 ==="
grep -h 'LOG_SESSION_CLOSE' /tmp/fly_boot.log /tmp/*.log 2>/dev/null | tail -3
