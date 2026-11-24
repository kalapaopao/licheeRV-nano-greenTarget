#!/bin/sh
# 补两组，凑齐四个象限：
#   D 纯采集（不检测不编码）
#   A' 只录像（连续模式，不检测）
cd /code/fly_LicheeRV || exit 1

run_case() {
    TAG=$1; shift
    sh stop.sh >/dev/null 2>&1
    sleep 1
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
        case "$c" in *fly_LicheeRV/camera*) kill "$pid" 2>/dev/null ;; esac
    done
    sleep 1
    cp config.yaml /tmp/c_$TAG.yaml
    for kv in "$@"; do
        k=${kv%%=*}; v=${kv#*=}
        sed -i "s|^$k: .*|$k: $v|" /tmp/c_$TAG.yaml
    done
    rm -rf record/sess_*
    echo ""
    echo "######## $TAG ########"
    grep -E '^(detect_enable|track_enable|record_enable|display_enable|uart_enable|log_mode|run_seconds)' /tmp/c_$TAG.yaml | sed 's/^/  /'
    FLY_CONFIG_FILE=/tmp/c_$TAG.yaml sh start.sh 26 > /tmp/run_$TAG.log 2>&1 &
    SUP=$!
    sleep 14
    PID=$(pidof camera 2>/dev/null | awk '{print $1}')
    if [ -n "$PID" ]; then
        A=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null)
        C0=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
        sleep 2
        B=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null)
        C1=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
        HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
        echo "$A $B $C0 $C1 $HZ" | awk '{printf "  camera CPU=%.0f%%  整机 CPU=%.0f%%\n", ($2-$1)*100/($5*2), ($4-$3)*100/($5*2)}'
    fi
    wait $SUP 2>/dev/null
    sh stop.sh >/dev/null 2>&1
    sleep 1
    echo "  性能统计："
    grep '^capture_frames=' /tmp/fly_licheerv_capture.log 2>/dev/null | tail -1 | sed 's/^/    /'
    S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
    [ -n "$S" ] && grep -E '^(rows_frames|video_frames|close_reason)' "$S/meta.txt" | sed 's/^/    /'
}

# D: 纯采集，不检测、不编码
run_case D_pure_capture detect_enable=false track_enable=false uart_enable=false \
         record_enable=false display_enable=false

# A': 只录像（连续模式），不检测
run_case A2_record_only detect_enable=false track_enable=false uart_enable=false \
         record_enable=true display_enable=false log_mode=continuous log_max_seconds=25
