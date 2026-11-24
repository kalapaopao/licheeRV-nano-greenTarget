#!/bin/sh
# 背靠背 A/B（时长 25s >= 最低 20s），保留两个会话，各抽一帧供本地分析
cd /code/fly_LicheeRV || exit 1
sh stop.sh >/dev/null 2>&1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in *fly_LicheeRV/camera*|*fake_flight*) kill "$pid" 2>/dev/null ;; esac
done
sleep 1
rm -rf record/sess_*

nohup python3 /tmp/fake_flight.py 12 40 5 > /tmp/fk_ab.log 2>&1 &
PTY=""
i=0
while [ $i -lt 12 ]; do
    PTY=$(head -1 /tmp/fk_ab.log 2>/dev/null | awk '{print $2}')
    [ -n "$PTY" ] && break
    sleep 1
    i=$((i + 1))
done
[ -n "$PTY" ] || { echo "假电控失败"; exit 1; }
echo "PTY=$PTY  (12s 静止 -> 40s 飞行)"

run_one() {
    CW=$1
    TAG=$2
    sed -e "s|^uart_device: .*|uart_device: $PTY|" \
        -e "s|^cam_crop_width: .*|cam_crop_width: $CW|" \
        -e 's|^auto_restart: .*|auto_restart: false|' \
        -e 's|^exit_after_land: .*|exit_after_land: false|' \
        -e 's|^run_seconds: .*|run_seconds: 25|' \
        config.yaml > /tmp/c_$TAG.yaml
    echo ""
    echo "==== $TAG (cam_crop_width=$CW) ===="
    grep -n 'cam_crop_width' /tmp/c_$TAG.yaml | sed 's/^/  cfg: /'
    FLY_CONFIG_FILE=/tmp/c_$TAG.yaml sh start.sh > /tmp/run_$TAG.log 2>&1
    echo "  rc=$?"
    head -3 /tmp/run_$TAG.log | sed 's/^/  log: /'
    grep -E 'FRAME_GEOM|VI_CROP|CALIB_CX' /tmp/fly_licheerv_capture.log 2>/dev/null | sed 's/^/  /'
    S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
    if [ -n "$S" ]; then
        echo "  会话: $(basename $S)"
        cp "$S/video.avi" "/tmp/ab_$TAG.avi"
        python3 /tmp/avid.py "/tmp/ab_$TAG.avi" | sed 's/^/  /'
        python3 /tmp/daf.py "/tmp/ab_$TAG.avi" 60 "/tmp/ab_$TAG.jpg" >/dev/null 2>&1 \
            && ls -l "/tmp/ab_$TAG.jpg" | sed 's/^/  frame: /'
    else
        echo "  (无会话)"
    fi
}

run_one 0    full
run_one 1104 crop
echo ""
echo "产物: /tmp/ab_full.avi  /tmp/ab_crop.avi  /tmp/ab_full.jpg  /tmp/ab_crop.jpg"
