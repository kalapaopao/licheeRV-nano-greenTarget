#!/bin/sh
# 真实场景对照：相机先跑 ~25 秒（预录缓冲填满 3 秒），然后才起飞。
# 这时 preroll=3000 会在触发瞬间 flush ~15-20MB，才复现真实的 1~2.5 秒空档。
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_mcu*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1

run_case() {
    VP=$1
    TAG=$2
    echo ""
    echo "################ video_preroll_ms=$VP  ($TAG) ################"

    # 60 秒一轮：30s 静止 / 18s 飞行 / 12s 停止
    # 相机起来约 5 秒，所以起飞发生在相机跑了 ~25 秒时 —— 预录缓冲已满
    sed "s|^video_preroll_ms: .*|video_preroll_ms: $VP|" config.yaml > /tmp/cfg_$TAG.yaml
    sed -i "s|^exit_after_land: .*|exit_after_land: false|" /tmp/cfg_$TAG.yaml
    sed -i "s|^run_seconds: .*|run_seconds: 300|" /tmp/cfg_$TAG.yaml
    sed -i "s|^log_max_seconds: .*|log_max_seconds: 120|" /tmp/cfg_$TAG.yaml

    rm -rf record/sess_*
    # 同时启动假电控与相机：相机 ready≈5s，起飞在假电控 30s → 相机已跑 25s
    nohup python3 /tmp/fake_mcu_pty.py 130 60 > /tmp/fk_$TAG.log 2>&1 &
    sleep 0.5
    PTY=$(head -1 /tmp/fk_$TAG.log | awk '{print $2}')
    [ -n "$PTY" ] || { echo "  假电控没起来"; return 1; }
    sed -i "s|^uart_device: .*|uart_device: $PTY|" /tmp/cfg_$TAG.yaml
    echo "  假电控: $PTY"

    FLY_CONFIG_FILE=/tmp/cfg_$TAG.yaml sh start.sh 300 > /tmp/run_$TAG.log 2>&1 &
    sleep 50
    sh stop.sh >/dev/null 2>&1
    sleep 2

    echo "  --- VIDEO_PREROLL 打点 ---"
    grep -E 'VIDEO_PREROLL' /tmp/fly_licheerv_capture.log 2>/dev/null | tail -2 | sed 's/^/    /'
    S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
    if [ -z "$S" ]; then echo "  没有生成会话"; return 1; fi
    grep -E '^(rows_frames|video_frames|close_reason)' "$S/meta.txt" | sed 's/^/    /'
    echo "  --- 空档分析 ---"
    python3 /tmp/cg1.py "$S"
}

run_case 3000 withpre
run_case 0    nopre
