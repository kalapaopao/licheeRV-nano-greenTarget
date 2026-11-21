#!/bin/sh
# 找相机进程的日志，确认它自己收到了多少电控帧
cd /code/fly_LicheeRV || exit 1

echo "=== start.sh 里的 LOG 变量 ==="
grep -E '^LOG=' start.sh

echo
echo "=== /tmp 下最近的日志 ==="
ls -lt /tmp/*.log 2>/dev/null | head -5

echo
for f in /tmp/fly_licheerv_capture.log /tmp/fly_camera.log /tmp/camera.log; do
    [ -f "$f" ] || continue
    echo "--- $f ---"
    grep -E 'UART sent=|UART:|\[MCU\]' "$f" | tail -4
done
