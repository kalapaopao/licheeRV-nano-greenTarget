#!/bin/sh
# 关键对照：相机运行时再扫一遍引脚电平
#   如果 A19 在跳 → UART1_TX 复用正常，我们的数据真的发出去了
#   如果 A19 静止 → TX 也没复用，那电控根本收不到我们的数据
cd /code/fly_LicheeRV || exit 1

sh stop.sh >/dev/null 2>&1
sleep 1
echo "=== 相机停止时（基线）==="
python3 /tmp/gsp.py 2

echo
echo "=== 启动相机（开始 50Hz 发帧），等 3 秒后扫描 ==="
sh start.sh 20 > /tmp/mux_run.log 2>&1 &
sleep 4
python3 /tmp/gsp.py 3

echo
echo "=== 程序自己的发送计数 ==="
grep -h 'UART sent=' /tmp/mux_run.log | tail -1
echo
echo "=== 温度 ==="
cat /sys/class/thermal/thermal_zone0/temp
sleep 12
