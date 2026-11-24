#!/bin/sh
# 分析板上最新一个会话（真实电控触发）
cd /code/fly_LicheeRV || exit 1
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
[ -n "$S" ] || { echo "没有会话"; exit 0; }
echo "会话: $S"
echo "=== meta ==="
grep -E '^(close_reason|rows_frames|video_frames|cfg )' "$S/meta.txt"
echo "=== 空档分析 ==="
python3 /tmp/cg1.py "$S"
echo "=== 预录打点 ==="
grep -E 'VIDEO_PREROLL' /tmp/fly_licheerv_capture.log 2>/dev/null | tail -2
