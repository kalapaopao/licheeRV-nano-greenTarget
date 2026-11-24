#!/bin/sh
# 验证 auto_restart：run_seconds 到点后是否自动重启；stop.sh 是否能阻止重启
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_flight*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

echo "############ 用例 1：run_seconds=25 到点后自动重启（max=2）############"
sed -e 's|^auto_restart: .*|auto_restart: true|' \
    -e 's|^auto_restart_max: .*|auto_restart_max: 2|' \
    -e 's|^auto_restart_delay_s: .*|auto_restart_delay_s: 2|' \
    -e 's|^exit_after_land: .*|exit_after_land: false|' \
    -e 's|^run_seconds: .*|run_seconds: 25|' \
    -e 's|^auto_restart_min_run_s: .*|auto_restart_min_run_s: 0|' \
    config.yaml > /tmp/c_ar.yaml
grep -E '^(auto_restart|auto_restart_max|auto_restart_delay_s|run_seconds|auto_restart_min_run_s)' /tmp/c_ar.yaml | sed 's/^/  /'

rm -f state/restart_short
echo "  --- 启动（不传时长，用 config 的 25 秒）---"
FLY_CONFIG_FILE=/tmp/c_ar.yaml sh start.sh > /tmp/ar_run.log 2>&1
echo "  最终退出 rc=$?"
echo "  --- AUTO_RESTART 事件 ---"
grep -E 'AUTO_RESTART|TEST_COMPLETED|CAMERA_READY_WAIT' /tmp/ar_run.log | sed 's/^/    /'
echo "  --- 重启轮数统计 ---"
printf '    AUTO_RESTART round 出现 %s 次\n' "$(grep -c 'AUTO_RESTART round=' /tmp/ar_run.log)"

echo
echo "############ 用例 2：stop.sh 必须能阻止重启 ############"
sed -e 's|^auto_restart: .*|auto_restart: true|' \
    -e 's|^auto_restart_max: .*|auto_restart_max: 0|' \
    -e 's|^run_seconds: .*|run_seconds: 0|' \
    config.yaml > /tmp/c_ar2.yaml
FLY_CONFIG_FILE=/tmp/c_ar2.yaml sh start.sh > /tmp/ar_run2.log 2>&1 &
SUP=$!
sleep 12
echo "  运行中，执行 stop.sh"
sh stop.sh >/dev/null 2>&1
sleep 6
if kill -0 $SUP 2>/dev/null; then
    echo "  ❌ stop 后仍有进程存活（PID $SUP）"
    kill -9 $SUP 2>/dev/null
else
    echo "  ✅ stop.sh 后进程已退出"
fi
if grep -q 'AUTO_RESTART round=' /tmp/ar_run2.log; then
    echo "  ❌ 被 stop 之后仍然重启了"
    grep 'AUTO_RESTART' /tmp/ar_run2.log | sed 's/^/    /'
else
    echo "  ✅ 被 stop 之后没有重启（符合预期）"
fi
grep -E 'AUTO_RESTART_SKIP|AUTO_RESTART_DISABLED' /tmp/ar_run2.log | tail -2 | sed 's/^/    /'
