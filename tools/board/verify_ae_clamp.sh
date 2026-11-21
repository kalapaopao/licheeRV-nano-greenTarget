#!/bin/sh
# 验证 auto 模式的曝光被夹到 10ms 以内，帧率和 JPEG 体积是否恢复
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
sleep 1
grep -E '^(exposure_mode|auto_exposure_max_us|jpeg_quality)' config.yaml

rm -f /tmp/ae.log
FLY_CONFIG_FILE=/code/fly_LicheeRV/config.yaml sh start.sh 30 > /tmp/ae.log 2>&1 &
sleep 26

echo
echo "=== AE 收敛值（关键看 exp_us）==="
grep -E 'RECORD80_AE_INFO|AE_EXP_CLAMP' /tmp/fly_licheerv_capture.log | tail -5

echo
echo "=== 帧率 / 编码 ==="
grep -E '^capture_frames=' /tmp/fly_licheerv_capture.log | tail -3

echo
echo "=== 温度 ==="
cat /sys/class/thermal/thermal_zone0/temp
sleep 8
