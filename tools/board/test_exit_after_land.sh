#!/bin/sh
# 验证 exit_after_land：假电控制造 STATIC→FLIGHT→STOP，程序应在归档后自己退出
set -u
cd /code/fly_LicheeRV || exit 1

sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_mcu*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

nohup python3 /tmp/fake_mcu_pty.py 45 > /tmp/fake.log 2>&1 &
sleep 1.5
PTY=$(head -1 /tmp/fake.log | awk '{print $2}')
echo "假电控: $PTY"

# 临时配置：指向假电控 + 打开 exit_after_land
sed "s|^uart_device: .*|uart_device: $PTY|" config.yaml > /tmp/cfg_exit.yaml
sed -i 's|^exit_after_land: .*|exit_after_land: true|' /tmp/cfg_exit.yaml
grep -E '^(uart_device|exit_after_land|record_enable)' /tmp/cfg_exit.yaml

echo
echo "=== 跑 start.sh（不指定秒数=常驻），预期落地后自己退出 ==="
T0=$(cut -d. -f1 /proc/uptime)
FLY_CONFIG_FILE=/tmp/cfg_exit.yaml sh start.sh > /tmp/exit_test.log 2>&1
T1=$(cut -d. -f1 /proc/uptime)
echo "start.sh 退出，耗时 $((T1 - T0)) 秒"

echo
echo "=== 日志关键行 ==="
grep -E 'EXIT_AFTER_LAND|CAMERA_EXIT_AFTER_LAND|LOG_SESSION|CAMERA_CRASH' /tmp/exit_test.log | head -6

echo
echo "=== 会话 ==="
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
[ -n "${S:-}" ] && grep -E '^(close_reason|rows_frames|video_frames)' "$S/meta.txt" | sed 's/^/  /'

echo
echo "=== run_history 尾部 ==="
tail -4 state/run_history.log
