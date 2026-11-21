#!/bin/sh
# 对照实验：A19（我们的 TX，应当有跳变） vs A18（电控该来数据的 RX）
#   A19 有跳变、A18 没有 → 探针有效、TX 焊盘是活的、RX 线上确实没信号
set -u
GPIO_TX=371      # GPIOA19
GPIO_RX=370      # GPIOA18
BASE=/sys/class/gpio

export_pin() {
    [ -d "$BASE/gpio$1" ] || { echo $1 > "$BASE/export" 2>/dev/null; sleep 0.3; }
    echo in > "$BASE/gpio$1/direction" 2>/dev/null
}
unexport_pin() { echo $1 > "$BASE/unexport" 2>/dev/null || true; }

echo "=== 先起相机，让 TX 上有数据 ==="
cd /code/fly_LicheeRV || exit 1
(sh start.sh 20 > /tmp/probe_run.log 2>&1 &)
sleep 10
echo "  camera: $(ps | grep -v grep | grep -c fly_LicheeRV/camera) 个进程"

export_pin $GPIO_TX
export_pin $GPIO_RX

echo
echo "--- GPIOA19（我们的 TX）---"
python3 /tmp/probe_pin.py 2 $GPIO_TX

echo
echo "--- GPIOA18（电控该来的 RX）---"
python3 /tmp/probe_pin.py 2 $GPIO_RX

unexport_pin $GPIO_TX
unexport_pin $GPIO_RX

echo
echo "=== 程序自己的收发计数 ==="
grep -h 'UART sent=' /tmp/probe_run.log | tail -1
sleep 10
