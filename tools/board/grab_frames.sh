#!/bin/sh
# 从最新会话里均匀抽 5 帧，供本地挑亮度合适的那张量黑边
cd /code/fly_LicheeRV || exit 1
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
[ -n "$S" ] || { echo "no session"; exit 1; }
LINES=$(wc -l < "$S/video_frames.csv")
TOTAL=$((LINES - 1))
echo "session=$S frames=$TOTAL"
i=1
for f in 5 20 40 60 80; do
    N=$((TOTAL * f / 100))
    python3 /tmp/daf.py "$S/video.avi" "$N" "/tmp/fr_$i.jpg" || true
    i=$((i + 1))
done
ls -l /tmp/fr_*.jpg 2>/dev/null
