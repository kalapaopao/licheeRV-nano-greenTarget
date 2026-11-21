#!/bin/sh
# B) 假电控触发一次：用假电控实际分配的 PTY，生成临时配置，跑一轮
set -u
cd /code/fly_LicheeRV || exit 1

# 停掉旧的
sh stop.sh >/dev/null 2>&1 || true
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in
        *fly_LicheeRV/camera*|*fake_mcu_pty*) kill "$pid" 2>/dev/null ;;
    esac
done
sleep 2

nohup python3 /tmp/fake_mcu_pty.py 40 > /tmp/fake.log 2>&1 &
sleep 1.5
PTY=$(head -1 /tmp/fake.log | awk '{print $2}')
echo "假电控 PTY = $PTY"

sed "s|^uart_device: .*|uart_device: $PTY|" config.yaml > config_rec.yaml
grep -E '^(uart_device|log_enable|record_enable)' config_rec.yaml | sed 's/^/  /'

rm -rf record/sess_*
FLY_CONFIG_FILE=/code/fly_LicheeRV/config_rec.yaml sh start.sh 24 > /tmp/b_trig.log 2>&1
sleep 1

echo
echo "record/ 结构："
find record -maxdepth 2 | sort | sed 's/^/  /'
s=$(ls -1d record/sess_* 2>/dev/null | tail -1)
if [ -n "${s:-}" ]; then
    echo
    echo "会话目录 $s："
    ls -l "$s" | awk 'NR>1{printf "  %8s  %s\n", $5, $NF}'
    echo
    grep -E '^(close_reason|rows_frames|video_frames|video_last_avi)' "$s/meta.txt" | sed 's/^/  /'
else
    echo
    echo "!! 没有会话目录，看日志："
    grep -E 'UART|LOG_SESSION|MCU' /tmp/b_trig.log | tail -5
fi
