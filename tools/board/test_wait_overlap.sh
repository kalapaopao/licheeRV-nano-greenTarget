#!/bin/sh
# 让「时长到点」真的落在飞行中间
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_flight*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

nohup python3 /tmp/fake_flight.py 12 20 5 > /tmp/fk_o.log 2>&1 &
sleep 1.5
PTY=$(head -1 /tmp/fk_o.log | awk '{print $2}')
echo "假电控: $PTY (12s takeoff, flight 20s -> land 32s)"
echo "start.sh duration=20s -> expires ~28s, i.e. mid-flight"

sed -e "s|^uart_device: .*|uart_device: $PTY|" \
    -e 's|^auto_restart: .*|auto_restart: true|' \
    -e 's|^auto_restart_max: .*|auto_restart_max: 5|' \
    -e 's|^auto_restart_delay_s: .*|auto_restart_delay_s: 2|' \
    -e 's|^auto_restart_wait_session_s: .*|auto_restart_wait_session_s: 60|' \
    -e 's|^exit_after_land: .*|exit_after_land: false|' \
    -e 's|^auto_restart_min_run_s: .*|auto_restart_min_run_s: 0|' \
    config.yaml > /tmp/c_o.yaml
rm -rf record/sess_* state/restart_short

FLY_CONFIG_FILE=/tmp/c_o.yaml sh start.sh 20 > /tmp/o.log 2>&1 &
SUP=$!
sleep 50
kill -TERM $SUP 2>/dev/null
sleep 4
sh stop.sh >/dev/null 2>&1
sleep 2

echo
echo "=== AUTO_RESTART events ==="
grep -E 'TEST_COMPLETED|AUTO_RESTART' /tmp/o.log | sed 's/^/  /'
echo
echo "=== session ==="
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
if [ -n "$S" ]; then
    echo "  $(basename $S) : $(grep '^close_reason' $S/meta.txt)"
    awk -F, 'NR>1{c[$24]++} END{for(k in c) printf "    state=%s: %d rows\n", k, c[k]}' "$S/frames.csv"
else
    echo "  (no session)"
fi
echo
echo "=== verdict ==="
if grep -q 'AUTO_RESTART_WAIT_SESSION' /tmp/o.log; then
    echo "  PASS: waited for session to finish before restarting"
    grep -E 'AUTO_RESTART_(WAIT_SESSION|SESSION_WAITED)' /tmp/o.log | sed 's/^/    /'
else
    echo "  WARN: still no overlap"
fi
