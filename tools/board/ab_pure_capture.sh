#!/bin/sh
# 补齐基线：
#   P1 纯采集（什么都不开）        -> 上限是多少
#   P2 只开预览编码（不写 SD）     -> 硬件 JPEG 本身的代价
cd /code/fly_LicheeRV || exit 1

run_case() {
    TAG=$1; shift
    sh stop.sh >/dev/null 2>&1
    sleep 1
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
        case "$c" in *fly_LicheeRV/camera*) kill -9 "$pid" 2>/dev/null ;; esac
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
    grep -E '^(detect_enable|track_enable|record_enable|display_enable|uart_enable|log_enable|run_seconds)' /tmp/c_$TAG.yaml | sed 's/^/  /'
    FLY_CONFIG_FILE=/tmp/c_$TAG.yaml sh start.sh 24 > /tmp/run_$TAG.log 2>&1
    echo "  rc=$?"
    echo "  性能统计（取 3 条，看是否稳定）："
    grep '^capture_frames=' /tmp/fly_licheerv_capture.log 2>/dev/null | tail -3 | sed 's/^/    /'
}

# P1：纯采集。日志关掉就不受 log_mode=trigger 的 uart 守卫限制
run_case P1_pure_capture detect_enable=false track_enable=false uart_enable=false \
         record_enable=false display_enable=false log_enable=false

# P2：只开预览编码（JPEG 硬件编码，但不写 SD 卡）
run_case P2_encode_only detect_enable=false track_enable=false uart_enable=false \
         record_enable=false display_enable=true log_enable=false
