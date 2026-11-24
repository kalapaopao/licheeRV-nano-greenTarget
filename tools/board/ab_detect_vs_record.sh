#!/bin/sh
# 三组对照，量化「检测」和「录像」各自吃掉多少帧率
#   A: 检测关 + 录像开   -> 录像开销
#   B: 检测开 + 录像关   -> 检测开销
#   C: 检测开 + 录像开   -> 完整体
cd /code/fly_LicheeRV || exit 1

run_case() {
    DET=$1; REC=$2; TAG=$3
    sh stop.sh >/dev/null 2>&1
    sleep 1
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
        case "$c" in *fly_LicheeRV/camera*|*fake_flight*) kill "$pid" 2>/dev/null ;; esac
    done
    sleep 1

    nohup python3 /tmp/fake_flight.py 10 14 4 > /tmp/fk_$TAG.log 2>&1 &
    PTY=""
    i=0
    while [ $i -lt 12 ]; do
        PTY=$(head -1 /tmp/fk_$TAG.log 2>/dev/null | awk '{print $2}')
        [ -n "$PTY" ] && break
        sleep 1
        i=$((i + 1))
    done
    [ -n "$PTY" ] || { echo "  假电控失败"; return 1; }

    sed -e "s|^uart_device: .*|uart_device: $PTY|" \
        -e "s|^detect_enable: .*|detect_enable: $DET|" \
        -e "s|^track_enable: .*|track_enable: $DET|" \
        -e "s|^record_enable: .*|record_enable: $REC|" \
        -e 's|^display_enable: .*|display_enable: false|' \
        -e 's|^auto_restart: .*|auto_restart: false|' \
        -e 's|^exit_after_land: .*|exit_after_land: false|' \
        -e 's|^run_seconds: .*|run_seconds: 26|' \
        config.yaml > /tmp/c_$TAG.yaml
    rm -rf record/sess_*

    echo ""
    echo "######## $TAG  (detect=$DET record=$REC) ########"
    FLY_CONFIG_FILE=/tmp/c_$TAG.yaml sh start.sh 26 > /tmp/run_$TAG.log 2>&1 &
    SUP=$!
    sleep 16
    # 飞行中采一次 CPU
    PID=$(pidof camera 2>/dev/null | awk '{print $1}')
    if [ -n "$PID" ]; then
        A=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null)
        C0=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
        sleep 2
        B=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null)
        C1=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
        HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
        echo "$A $B $C0 $C1 $HZ" | awk '{
          printf "  camera CPU=%.0f%%   整机 CPU=%.0f%%\n", ($2-$1)*100/($5*2), ($4-$3)*100/($5*2)
        }'
    fi
    wait $SUP 2>/dev/null
    sh stop.sh >/dev/null 2>&1
    sleep 1

    # 取飞行期间最后一条性能统计行
    L=/tmp/fly_licheerv_capture.log
    echo "  性能统计（最后一条 capture_frames 行）："
    grep '^capture_frames=' "$L" 2>/dev/null | tail -1 \
      | sed 's/ duplicate_pts=[0-9]* timeouts=[0-9]*/\n    /' | sed 's/^/    /'
    echo "  检测统计（若有）："
    grep '^DETECT ' "$L" 2>/dev/null | tail -1 | cut -c1-110 | sed 's/^/    /'
    S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
    if [ -n "$S" ]; then
        grep -E '^(rows_frames|video_frames|close_reason)' "$S/meta.txt" | sed 's/^/    /'
    else
        echo "    (无会话)"
    fi
}

run_case false true  A_rec_only
run_case true  false B_det_only
run_case true  true  C_both
