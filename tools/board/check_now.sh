#!/bin/sh
# 现在启动一次，验证能不能收到电控数据
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
sleep 1

echo "=== 启动前 pinmux（默认应该是 4）==="
echo "  A18=$(devmem 0x03001068 32)  A19=$(devmem 0x03001064 32)"

rm -f /tmp/now.log
FLY_CONFIG_FILE=/code/fly_LicheeRV/config.yaml sh start.sh 22 > /tmp/now.log 2>&1 &
sleep 12

echo
echo "=== 启动后 pinmux（start.sh 应自动改成 6）==="
echo "  A18=$(devmem 0x03001068 32)  A19=$(devmem 0x03001064 32)"

echo
echo "=== 串口统计行（rx_frames 就是收到的电控帧数）==="
grep -E 'UART sent=' /tmp/fly_licheerv_capture.log | tail -2

echo
echo "=== 电控帧打印（最近 2 条）==="
grep -E '\[MCU\]' /tmp/fly_licheerv_capture.log | tail -2

echo
echo "=== 温度 ==="
cat /sys/class/thermal/thermal_zone0/temp
sleep 12
