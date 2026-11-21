#!/bin/sh
# 最终验证：相机进程自己能否收到电控帧（rx_frames 增长），以及 A19 是否真的在输出
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
sleep 1

rm -f /tmp/ver.log
FLY_CONFIG_FILE=/code/fly_LicheeRV/config.yaml sh start.sh 24 > /tmp/ver.log 2>&1 &
sleep 12

echo "=== 相机进程的串口统计（rx_frames 是关键）==="
grep -E 'UART sent=' /tmp/ver.log | tail -3

echo
echo "=== 电控帧打印（[MCU] 行）==="
grep -E '\[MCU\]' /tmp/ver.log | tail -3

echo
echo "=== A19（我们的 TX）现在是 UART1_TX，相机在跑时应该在跳 ==="
python3 /tmp/gsp.py 2

sleep 12
echo
echo "=== 内核最终计数 ==="
grep -E '^1: ' /proc/tty/driver/serial
