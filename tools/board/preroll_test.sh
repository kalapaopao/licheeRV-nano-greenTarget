#!/bin/sh
# 预录验证：静止 N 秒确保相机跑够久，再起飞，检查 frames.csv 里有没有 STATIC 预录行
# 用法: sh /tmp/preroll_test.sh <静止秒> <飞行秒>
cd /code/fly_LicheeRV || exit 1
STATIC_S=${1:-25}
FLIGHT_S=${2:-15}
DUR=$((STATIC_S + FLIGHT_S + 25))

sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_flight*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

echo "=== 静止 ${STATIC_S}s 然后飞行 ${FLIGHT_S}s；相机跑 ${DUR}s ==="
nohup python3 /tmp/fake_flight.py "$STATIC_S" "$FLIGHT_S" 5 > /tmp/ff2.log 2>&1 &
sleep 1.5
PTY=$(head -1 /tmp/ff2.log | awk '{print $2}')
[ -n "$PTY" ] || { echo "假电控失败: $(cat /tmp/ff2.log)"; exit 1; }
echo "  假电控: $PTY"

sed "s|^uart_device: .*|uart_device: $PTY|" config.yaml > /tmp/c2.yaml
rm -rf record/sess_*
FLY_CONFIG_FILE=/tmp/c2.yaml sh start.sh "$DUR" > /tmp/r2.log 2>&1
echo "  start.sh rc=$?"

S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
[ -n "$S" ] || { echo "  ❌ 没有会话"; exit 1; }
echo "  会话: $S"
grep -E '^(close_reason|rows_frames|video_frames)' "$S/meta.txt" | sed 's/^/    /'

echo
echo "=== state 分布（0=STATIC 应该是预录那段）==="
awk -F, 'NR>1{c[$24]++} END{for(k in c) printf "  state=%s : %d 行\n", k, c[k]}' "$S/frames.csv"

echo
echo "=== 首帧相对触发时刻 ==="
TRIG=$(grep '^trigger_mono_us' "$S/meta.txt" | cut -d= -f2)
FIRST=$(awk -F, 'NR==2{print $2}' "$S/frames.csv")
awk -v t="$TRIG" -v f="$FIRST" 'BEGIN{printf "  trigger=%s  首帧=%s  首帧-触发=%+.3f s\n", t, f, (f-t)/1e6}'

echo
echo "=== 前 2 行 / 后 2 行 ==="
awk -F, 'NR>1 && NR<=3{printf "  fid=%-6s state=%s\n", $1, $24}' "$S/frames.csv"
tail -2 "$S/frames.csv" | awk -F, '{printf "  fid=%-6s state=%s\n", $1, $24}'
