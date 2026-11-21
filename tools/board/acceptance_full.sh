#!/bin/sh
# 端到端验收：start.sh 自动改 pinmux → 收到电控帧 → 落地记录
cd /code/fly_LicheeRV || exit 1

sh stop.sh >/dev/null 2>&1
sleep 1

echo "=== ① 启动前：pinmux 恢复成默认(4) ==="
devmem 0x03001068 32 0x4 >/dev/null 2>&1
devmem 0x03001064 32 0x4 >/dev/null 2>&1
echo "  A18=$(devmem 0x03001068 32)  A19=$(devmem 0x03001064 32)   (4=CTS/RTS 默认)"

echo
echo "=== ② 跑 start.sh（它应该自动把 A18/A19 改成 6）==="
FLY_CONFIG_FILE=/code/fly_LicheeRV/config.yaml sh start.sh 26 > /tmp/final_run.log 2>&1 &
sleep 10

echo "  A18=$(devmem 0x03001068 32)  A19=$(devmem 0x03001064 32)   (6=UART1 RX/TX)"

echo
echo "=== ③ 日志关键行 ==="
grep -E 'UART1_PINMUX|UART:|UART sent=|\[MCU\]' /tmp/final_run.log | head -6

echo
echo "=== ④ 内核 rx 计数 ==="
grep -E '^1: ' /proc/tty/driver/serial

echo
echo "=== ⑤ 有没有生成会话目录 ==="
ls -1 record/ 2>/dev/null | tail -3

sleep 18
echo
echo "=== ⑥ 收尾后（会话应已关闭）==="
S=$(ls -1d record/sess_* 2>/dev/null | tail -1)
if [ -n "${S:-}" ]; then
    echo "  会话: $S"
    ls -l "$S" | awk 'NR>1{printf "     %9s  %s\n", $5, $NF}'
    grep -E '^(close_reason|rows_frames|mcu_stream)' "$S/meta.txt" | sed 's/^/     /'
    echo "  frames.csv 前两行："
    head -2 "$S/frames.csv" | cut -c1-140 | sed 's/^/     /'
else
    echo "  (没有会话目录)"
fi
