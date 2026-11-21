#!/bin/sh
# 看 auto 曝光现在收敛到什么值、以及帧率有没有被拖累
cd /code/fly_LicheeRV || exit 1

echo "=== 当前配置 ==="
grep -E '^(exposure_mode|manual_exposure_us|manual_analog_gain_x1024|auto_|jpeg_quality)' config.yaml

echo
echo "=== 日志里的曝光信息 ==="
grep -E 'RECORD80_AE|EXPOSURE|EXPOSURE_SET|exp=' /tmp/fly_licheerv_capture.log 2>/dev/null | tail -8

echo
echo "=== 最近的帧率/编码统计 ==="
grep -E '^capture_frames=' /tmp/fly_licheerv_capture.log 2>/dev/null | tail -3

echo
echo "=== 最近一次会话的视频体积/帧数（判断 JPEG 大小）==="
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
if [ -n "$S" ]; then
    VSZ=$(ls -l "$S/video.avi" 2>/dev/null | awk '{print $5}')
    VF=$(grep -o 'video_frames=[0-9]*' "$S/meta.txt" | head -1 | cut -d= -f2)
    echo "  会话: $S"
    echo "  video.avi = ${VSZ:-?} 字节, video_frames=${VF:-?}"
    [ -n "$VSZ" ] && [ -n "$VF" ] && [ "$VF" -gt 0 ] && \
        echo "  平均每帧 = $((VSZ / VF)) 字节"
fi
