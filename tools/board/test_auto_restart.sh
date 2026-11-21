#!/bin/sh
# 验证 auto_restart：假电控循环 20 秒一轮，程序应该在落地归档后自动重启，
# 直到达到 auto_restart_max 轮数才停。
set -u
cd /code/fly_LicheeRV || exit 1

MAXROUND=${1:-3}
sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_mcu*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

# 假电控：跑 200 秒，20 秒一轮（10s 静止 / 6s 飞行 / 4s 停止）
nohup python3 /tmp/fake_mcu_pty.py 200 20 > /tmp/fake2.log 2>&1 &
sleep 1.5
PTY=$(head -1 /tmp/fake2.log | awk '{print $2}')
echo "假电控: $PTY"

sed "s|^uart_device: .*|uart_device: $PTY|" config.yaml > /tmp/cfg_ar.yaml
sed -i 's|^exit_after_land: .*|exit_after_land: true|' /tmp/cfg_ar.yaml
sed -i 's|^auto_restart: .*|auto_restart: true|' /tmp/cfg_ar.yaml
sed -i "s|^auto_restart_max: .*|auto_restart_max: $MAXROUND|" /tmp/cfg_ar.yaml
sed -i 's|^auto_restart_delay_s: .*|auto_restart_delay_s: 3|' /tmp/cfg_ar.yaml
sed -i 's|^run_seconds: .*|run_seconds: 300|' /tmp/cfg_ar.yaml
grep -E '^(uart_device|exit_after_land|auto_restart|auto_restart_max|run_seconds)' /tmp/cfg_ar.yaml

echo
echo "=== 跑 start.sh（预期跑满 $MAXROUND 轮后自动停）==="
T0=$(cut -d. -f1 /proc/uptime)
FLY_CONFIG_FILE=/tmp/cfg_ar.yaml sh start.sh 300 > /tmp/ar_test.log 2>&1
RC=$?
T1=$(cut -d. -f1 /proc/uptime)
echo "start.sh 退出 rc=$RC，总耗时 $((T1 - T0)) 秒"

echo
echo "=== 关键事件时间线 ==="
grep -nE 'CAMERA_READY_WAIT|LOG_SESSION_OPEN|LOG_SESSION_CLOSE|AUTO_RESTART|CAMERA_EXIT_AFTER_LAND|AUTO_RESTART_STOP' /tmp/ar_test.log | sed 's/^/  /'

echo
echo "=== run_history 尾部（看每轮的 supervisor_exit reason）==="
tail -12 state/run_history.log

echo
echo "=== 生成了几段会话 ==="
ls -1dt record/sess_* 2>/dev/null | head -6
for S in $(ls -1dt record/sess_* 2>/dev/null | head -$MAXROUND); do
    printf '  %s: ' "$(basename "$S")"
    grep -E '^(close_reason|rows_frames|video_frames)' "$S/meta.txt" | tr '\n' ' '
    echo
done
