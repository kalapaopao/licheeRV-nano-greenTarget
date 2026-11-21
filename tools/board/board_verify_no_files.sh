#!/bin/sh
# 验证：「没触发 = 磁盘上零文件」；「触发后 = 一个文件夹里齐全」
set -u
cd /code/fly_LicheeRV || exit 1

stop_all() {
    sh stop.sh >/dev/null 2>&1 || true
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        if grep -q 'fly_LicheeRV/camera' "$p/cmdline" 2>/dev/null; then kill "$pid" 2>/dev/null; fi
    done
    sleep 2
}

stop_all
rm -rf record
mkdir -p record

echo "=== A) 只启动，不触发（20 秒）==="
(sh start.sh 20 > /tmp/a_idle.log 2>&1 &)
sleep 14
echo "record/ 内容："
ls -A record 2>/dev/null | sed 's/^/  /'
[ -z "$(ls -A record 2>/dev/null)" ] && echo "  (空 ✓)"
grep -E 'RECORDING_ARMED|RECORDING_OFF|WEB_READY|WEB_DISABLED' /tmp/a_idle.log | sed 's/^/  启动打印: /'
sleep 10

echo
echo "=== B) 假电控触发一次 ==="
stop_all
nohup python3 /tmp/fake_mcu_pty.py 40 > /tmp/fake.log 2>&1 &
sleep 1.5
sh /tmp/mk_rec_cfg.sh 2>/dev/null || true
FLY_CONFIG_FILE=/code/fly_LicheeRV/config_rec.yaml sh start.sh 24 > /tmp/b_trig.log 2>&1
sleep 1
echo "record/ 结构："
find record -maxdepth 2 | sort | sed 's/^/  /'
s=$(ls -1d record/sess_* 2>/dev/null | tail -1)
if [ -n "${s:-}" ]; then
    echo
    echo "会话目录 $s 内容："
    ls -l "$s" | awk 'NR>1{printf "  %8s  %s\n", $5, $NF}'
    echo
    grep -E '^(close_reason|rows_frames|video_frames|video_last_avi)' "$s/meta.txt" | sed 's/^/  /'
fi
