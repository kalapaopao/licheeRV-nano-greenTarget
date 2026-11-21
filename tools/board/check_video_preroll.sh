#!/bin/sh
# 验证视频是否包含预录：比较 frames.csv 与 video_frames.csv 的第一帧时刻
cd /code/fly_LicheeRV || exit 1
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
[ -n "$S" ] || { echo "没有会话目录"; exit 1; }
echo "会话: $S"
ls -l "$S" | awk 'NR>1{printf "  %10s %s\n", $5, $NF}'
echo
echo "--- meta 关键项 ---"
grep -E '^(close_reason|rows_frames|video_frames|video_first_avi|video_last_avi|rows_mcu)' "$S/meta.txt" | sed 's/^/  /'
echo
echo "--- 第一帧对比（capture_us 是第 10 列，frame_id 是第 1 列）---"
echo -n "  frames.csv 首行: "; head -2 "$S/frames.csv" | tail -1 | cut -d, -f1,10
echo -n "  frames.csv 末行: "; tail -1 "$S/frames.csv" | cut -d, -f1,10
echo -n "  video 首行:      "; head -2 "$S/video_frames.csv" | tail -1
echo -n "  video 末行:      "; tail -1 "$S/video_frames.csv"
echo
echo "--- 日志里的预录打点 ---"
grep -E 'VIDEO_PREROLL|LOG_SESSION_OPEN|LOG_SESSION_CLOSE' /tmp/fly_licheerv_capture.log | tail -4 | sed 's/^/  /'
