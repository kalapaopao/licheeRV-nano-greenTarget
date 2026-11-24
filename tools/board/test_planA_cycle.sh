#!/bin/sh
# 方案 A 端到端验证：落地 → 归档 → 程序退出 → 自动重启 → 第二次飞行
# 假电控 30 秒一轮（10s 静止 / 6s 飞行 / 4s 停止 / 10s 静止），跑 2 轮
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_mcu*|*fake_flight*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

echo "=== 本次配置（方案 A）==="
grep -E '^(run_seconds|exit_after_land|auto_restart|auto_restart_on_timeout|auto_restart_max)' config.yaml | sed 's/^/  /'

nohup python3 /tmp/fake_mcu_pty.py 120 30 > /tmp/fk_pa.log 2>&1 &
sleep 1.5
PTY=$(head -1 /tmp/fk_pa.log | awk '{print $2}')
[ -n "$PTY" ] || { echo "假电控没起来"; exit 1; }
echo "  假电控: $PTY (30s 一轮: 10s 静止 / 6s 飞行 / 4s 停止 / 10s 静止)"

sed "s|^uart_device: .*|uart_device: $PTY|" config.yaml > /tmp/c_pa.yaml
rm -rf record/sess_* state/restart_short

echo
echo "=== 跑 95 秒（应覆盖两次飞行 + 一次重启）==="
FLY_CONFIG_FILE=/tmp/c_pa.yaml sh start.sh > /tmp/pa.log 2>&1 &
SUP=$!
sleep 95
kill -TERM $SUP 2>/dev/null
sleep 4
sh stop.sh >/dev/null 2>&1
sleep 2

echo
echo "=== 关键事件 ==="
grep -E 'CAMERA_READY_WAIT|CAMERA_EXIT_AFTER_LAND|TEST_COMPLETED|AUTO_RESTART|TEMP_LIMIT|CAMERA_CRASH' /tmp/pa.log | sed 's/^/  /'

echo
echo "=== 会话列表 ==="
for S in $(ls -1dt record/sess_* 2>/dev/null); do
    printf '  %s : %s\n' "$(basename $S)" "$(grep '^close_reason' $S/meta.txt 2>/dev/null)"
    awk -F, 'NR>1{c[$24]++} END{for(k in c) printf "      state=%s: %d\n", k, c[k]}' "$S/frames.csv" 2>/dev/null
done

echo
echo "=== 判定 ==="
N=$(ls -1d record/sess_* 2>/dev/null | wc -l)
echo "  会话数: $N"
grep -q 'AUTO_RESTART round=' /tmp/pa.log && echo "  ✅ 发生了自动重启" || echo "  ❌ 没有自动重启"
grep -q 'CAMERA_EXIT_AFTER_LAND' /tmp/pa.log && echo "  ✅ 落地后程序退出（exit_after_land）" || echo "  ⚠ 没看到落地退出"
if [ "$N" -ge 2 ]; then echo "  ✅ 两次飞行各自成册"; else echo "  ⚠ 只录到 $N 次飞行"; fi
grep -q 'AUTO_RESTART_SKIP.*test_completed' /tmp/pa.log && echo "  （超时退出未重启：符合方案A）" || true
