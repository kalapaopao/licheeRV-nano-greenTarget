#!/bin/sh
# Standalone GC4653 / 80 FPS / 3000us; optional native MJPEG AVI recording.
set -eu
ROOT=/code/fly_LicheeRV
CONFIG=${FLY_CONFIG_FILE:-$ROOT/config.yaml}
# Auto-boot mode: use a dedicated always-on config, do not disturb the
# interactive config.yaml used for manual runs / tests.

# Limited flat-YAML reader: no eval/source, no nested structures, no silent typos.
config_error() { echo "CONFIG_ERROR: $*" >&2; exit 2; }
validate_integer() {
    key=$1; value=$2; low=$3; high=$4
    case "$value" in ''|*[!0-9]*) config_error "$key must be an integer ($low..$high)";; esac
    [ "$value" -ge "$low" ] && [ "$value" -le "$high" ] || config_error "$key outside $low..$high"
}
validate_auto_gain() {
    key=$1; value=$2; high=$3
    validate_integer "$key" "$value" 0 "$high"
    [ "$value" -eq 0 ] || [ "$value" -ge 1024 ] || config_error "$key must be 0 or at least 1024"
}
# 浮点校验：接受 -12.5 / 12.5 / .5 / 12 / -0.001；拒绝空串、纯符号、多个小数点。
# 不支持科学计数法（本配置不需要）。范围校验由 camera 的 env_f32 兜底。
validate_float() {
    key=$1; value=$2
    case "$value" in
        '') config_error "$key must be a number";;
        -*) rest=${value#-};;
        *)  rest=$value;;
    esac
    case "$rest" in
        ''|.|*[!0-9.]*|*.*.*) config_error "$key must be a number (got '$value')";;
    esac
}
EXPOSURE_MODE=manual
MANUAL_EXP_US=3000
MANUAL_AGAIN=1024
MANUAL_DGAIN=1024
MANUAL_ISPGAIN=1024
AUTO_EXP_MAX_US=0
AUTO_AGAIN_MAX=0
AUTO_DGAIN_MAX=0
AUTO_ISPGAIN_MAX=0
BLC_MODE=manual
BLC_R=256
BLC_GR=256
BLC_GB=256
BLC_B=256
JPEG_QUALITY=85
RUN_SECONDS=120
RECORD_ENABLE=false
RECORDING=0
WEB_BIND=auto
DISPLAY_ENABLE=true
# 网页服务（MJPEG 预览/抓图）。关掉可省 CPU，但录像不受影响
HTTP_ENABLE=true
DETECT_SAMPLE_STEPS=2
DETECT_STRIDE=1
DETECT_ENABLE=true
# OSD：把电控状态烧进画面左上角（录像里也能看到），默认关
OSD_ENABLE=false
OSD_SCALE=2
# ---- 阶段1：目标跟踪 + 角度 + 串口（默认值与 camera.cpp 保持一致）----
TRACK_ENABLE=true
CAM_FX=1023.2
CAM_FY=1022.2
CAM_CX=643.8
CAM_CY=382.8
OPTICAL_YAW_DEG=0.0
OPTICAL_PITCH_DEG=0.0
CAM_INVERT_YAW=false
CAM_INVERT_PITCH=false
TARGET_DIAMETER_M=0.05
TARGET_UP_OFFSET_FACTOR=1.5
SEL_CONF_WEIGHT=0.5
SEL_MAX_ASPECT=1.5
SEL_MAX_DIST_DEG=60.0
SEL_MAX_CANDIDATES=8
SEL_CONFIRM_FRAMES=5
SEL_LOST_FRAMES=10
KF_PROCESS_NOISE=50.0
KF_MEASURE_NOISE=0.001
KF_INIT_POS_VAR=10.0
KF_INIT_VEL_VAR=2.0
UART_ENABLE=false
UART_TX_ENABLE=true
UART_DEVICE=/dev/ttyS1
UART_BAUD=115200
UART_RATE_HZ=50
UART_FORMAT=ascii
UART_RX_PRINT=true
UART_RX_PRINT_HZ=10
# ---- 飞行数据记录（机型 default 与 camera.cpp 一致）----
LOG_ENABLE=false
LOG_MODE=trigger
LOG_DIR=
LOG_PREROLL_MS=3000
LOG_POSTROLL_MS=2000
LOG_MAX_SECONDS=30
LOG_MCU_STREAM=false
# ---- 诊断：分类打印开关（默认全开，等于与改动前一样）+ 运行状态历史目录 ----
STATE_DIR=
PRINT_ENABLE=true
PRINT_BOOT=true
PRINT_ISP=true
PRINT_TARGET=true
PRINT_PERF=true
PRINT_UART=true
PRINT_MCU=true
PRINT_LOG=true
PRINT_AVI=true

[ -r "$CONFIG" ] || config_error "missing/unreadable $CONFIG"
seen=' '
while IFS= read -r line || [ -n "$line" ]; do
    line=$(printf '%s' "$line" | tr -d '\r')
    line=${line%%#*}
    case "$line" in *[![:space:]]*) ;; *) continue;; esac
    case "$line" in *:*) ;; *) config_error "expected key: value in $CONFIG";; esac
    key=$(printf '%s' "${line%%:*}" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    value=$(printf '%s' "${line#*:}" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    [ -n "$value" ] || config_error "empty value for $key"
    case "$value" in *:*) config_error "nested/complex YAML is unsupported for $key";; esac
    case "$seen" in *" $key "*) config_error "duplicate key $key";; esac
    seen="$seen$key "
    case "$key" in
        exposure_mode) EXPOSURE_MODE=$value;;
        manual_exposure_us) MANUAL_EXP_US=$value;;
        manual_analog_gain_x1024) MANUAL_AGAIN=$value;;
        manual_digital_gain_x1024) MANUAL_DGAIN=$value;;
        manual_isp_gain_x1024) MANUAL_ISPGAIN=$value;;
        auto_exposure_max_us) AUTO_EXP_MAX_US=$value;;
        auto_analog_gain_max_x1024) AUTO_AGAIN_MAX=$value;;
        auto_digital_gain_max_x1024) AUTO_DGAIN_MAX=$value;;
        auto_isp_gain_max_x1024) AUTO_ISPGAIN_MAX=$value;;
        black_level_mode) BLC_MODE=$value;;
        black_level_r) BLC_R=$value;;
        black_level_gr) BLC_GR=$value;;
        black_level_gb) BLC_GB=$value;;
        black_level_b) BLC_B=$value;;
        jpeg_quality) JPEG_QUALITY=$value;;
        run_seconds) RUN_SECONDS=$value;;
        record_enable) RECORD_ENABLE=$value;;
        web_ip) WEB_BIND=$value;;
        display_enable) DISPLAY_ENABLE=$value;;
        http_enable) HTTP_ENABLE=$value;;
        detect_sample_steps) DETECT_SAMPLE_STEPS=$value;;
        detect_stride) DETECT_STRIDE=$value;;
        detect_enable) DETECT_ENABLE=$value;;
        osd_enable) OSD_ENABLE=$value;;
        osd_scale) OSD_SCALE=$value;;
        track_enable) TRACK_ENABLE=$value;;
        cam_fx) CAM_FX=$value;;
        cam_fy) CAM_FY=$value;;
        cam_cx) CAM_CX=$value;;
        cam_cy) CAM_CY=$value;;
        optical_yaw_deg) OPTICAL_YAW_DEG=$value;;
        optical_pitch_deg) OPTICAL_PITCH_DEG=$value;;
        cam_invert_yaw) CAM_INVERT_YAW=$value;;
        cam_invert_pitch) CAM_INVERT_PITCH=$value;;
        target_diameter_m) TARGET_DIAMETER_M=$value;;
        target_up_offset_factor) TARGET_UP_OFFSET_FACTOR=$value;;
        sel_conf_weight) SEL_CONF_WEIGHT=$value;;
        sel_max_aspect) SEL_MAX_ASPECT=$value;;
        sel_max_dist_deg) SEL_MAX_DIST_DEG=$value;;
        sel_max_candidates) SEL_MAX_CANDIDATES=$value;;
        sel_confirm_frames) SEL_CONFIRM_FRAMES=$value;;
        sel_lost_frames) SEL_LOST_FRAMES=$value;;
        kf_process_noise) KF_PROCESS_NOISE=$value;;
        kf_measure_noise) KF_MEASURE_NOISE=$value;;
        kf_init_pos_var) KF_INIT_POS_VAR=$value;;
        kf_init_vel_var) KF_INIT_VEL_VAR=$value;;
        uart_enable) UART_ENABLE=$value;;
        uart_device) UART_DEVICE=$value;;
        uart_baud) UART_BAUD=$value;;
        uart_rate_hz) UART_RATE_HZ=$value;;
        uart_format) UART_FORMAT=$value;;
        uart_rx_print) UART_RX_PRINT=$value;;
        uart_rx_print_hz) UART_RX_PRINT_HZ=$value;;
        uart_tx_enable) UART_TX_ENABLE=$value;;
        log_enable) LOG_ENABLE=$value;;
        log_mode) LOG_MODE=$value;;
        log_dir) LOG_DIR=$value;;
        log_preroll_ms) LOG_PREROLL_MS=$value;;
        log_postroll_ms) LOG_POSTROLL_MS=$value;;
        log_max_seconds) LOG_MAX_SECONDS=$value;;
        log_mcu_stream) LOG_MCU_STREAM=$value;;
        state_dir) STATE_DIR=$value;;
        print_enable) PRINT_ENABLE=$value;;
        print_boot) PRINT_BOOT=$value;;
        print_isp) PRINT_ISP=$value;;
        print_target) PRINT_TARGET=$value;;
        print_perf) PRINT_PERF=$value;;
        print_uart) PRINT_UART=$value;;
        print_mcu) PRINT_MCU=$value;;
        print_log) PRINT_LOG=$value;;
        print_avi) PRINT_AVI=$value;;
        *) config_error "unknown key $key";;
    esac
done < "$CONFIG"

case "$EXPOSURE_MODE" in manual|auto) ;; *) config_error 'exposure_mode must be manual or auto';; esac
case "$RECORD_ENABLE" in true|false) ;; *) config_error 'record_enable must be true or false';; esac
case "$WEB_BIND" in auto|usb|wifi) ;; *) config_error 'web_ip must be auto, usb or wifi';; esac
case "$DISPLAY_ENABLE" in true|false) ;; *) config_error 'display_enable must be true or false';; esac
case "$HTTP_ENABLE" in true|false) ;; *) config_error 'http_enable must be true or false';; esac
validate_integer detect_sample_steps "$DETECT_SAMPLE_STEPS" 1 4
validate_integer detect_stride "$DETECT_STRIDE" 1 10
case "$DETECT_ENABLE" in true|false) ;; *) config_error 'detect_enable must be true or false';; esac
case "$OSD_ENABLE" in true|false) ;; *) config_error 'osd_enable must be true or false';; esac
validate_integer osd_scale "$OSD_SCALE" 1 3
case "$BLC_MODE" in manual|default) ;; *) config_error 'black_level_mode must be manual or default';; esac

# ---- 阶段1：目标跟踪 / 标定 / 串口 校验 ----
case "$TRACK_ENABLE" in true|false) ;; *) config_error 'track_enable must be true or false';; esac
case "$CAM_INVERT_YAW" in true|false) ;; *) config_error 'cam_invert_yaw must be true or false';; esac
case "$CAM_INVERT_PITCH" in true|false) ;; *) config_error 'cam_invert_pitch must be true or false';; esac
case "$UART_ENABLE" in true|false) ;; *) config_error 'uart_enable must be true or false';; esac
# 帧格式：ascii = v1 文本帧（$GT,...）；binary = v2 定长二进制帧（A5 5A ... CRC16，28 字节）
case "$UART_FORMAT" in ascii|binary) ;; *) config_error 'uart_format must be ascii or binary';; esac
case "$UART_RX_PRINT" in true|false) ;; *) config_error 'uart_rx_print must be true or false';; esac
validate_integer uart_rx_print_hz "$UART_RX_PRINT_HZ" 1 100
validate_float cam_fx "$CAM_FX"
validate_float cam_fy "$CAM_FY"
validate_float cam_cx "$CAM_CX"
validate_float cam_cy "$CAM_CY"
validate_float optical_yaw_deg "$OPTICAL_YAW_DEG"
validate_float optical_pitch_deg "$OPTICAL_PITCH_DEG"
validate_float target_diameter_m "$TARGET_DIAMETER_M"
validate_float target_up_offset_factor "$TARGET_UP_OFFSET_FACTOR"
validate_float sel_conf_weight "$SEL_CONF_WEIGHT"
validate_float sel_max_aspect "$SEL_MAX_ASPECT"
validate_float sel_max_dist_deg "$SEL_MAX_DIST_DEG"
validate_float kf_process_noise "$KF_PROCESS_NOISE"
validate_float kf_measure_noise "$KF_MEASURE_NOISE"
validate_float kf_init_pos_var "$KF_INIT_POS_VAR"
validate_float kf_init_vel_var "$KF_INIT_VEL_VAR"
validate_integer sel_max_candidates "$SEL_MAX_CANDIDATES" 1 32
validate_integer sel_confirm_frames "$SEL_CONFIRM_FRAMES" 1 1000
validate_integer sel_lost_frames "$SEL_LOST_FRAMES" 1 1000
validate_integer uart_baud "$UART_BAUD" 1200 4000000
validate_integer uart_rate_hz "$UART_RATE_HZ" 1 200
case "$UART_DEVICE" in /dev/*) ;; *) config_error 'uart_device must start with /dev/';; esac
# 串口开着却没有跟踪链路 = 没有内容可发，直接拒绝，避免联调时白等
[ "$UART_ENABLE" = "true" ] && [ "$TRACK_ENABLE" != "true" ] && \
    config_error 'uart_enable requires track_enable=true'
# ---- 飞行数据记录 ----
case "$UART_TX_ENABLE" in true|false) ;; *) config_error 'uart_tx_enable must be true or false';; esac
case "$LOG_ENABLE" in true|false) ;; *) config_error 'log_enable must be true or false';; esac
case "$LOG_MODE" in trigger|continuous) ;; *) config_error 'log_mode must be trigger or continuous';; esac
validate_integer log_preroll_ms "$LOG_PREROLL_MS" 0 10000
validate_integer log_postroll_ms "$LOG_POSTROLL_MS" 0 10000
validate_integer log_max_seconds "$LOG_MAX_SECONDS" 1 300
case "$LOG_MCU_STREAM" in true|false) ;; *) config_error 'log_mcu_stream must be true or false';; esac
# trigger 模式靠电控 STATE 边沿触发；没有串口就永远不会有边沿，直接拒绝而不是静默空记
[ "$LOG_ENABLE" = "true" ] && [ "$LOG_MODE" = "trigger" ] && [ "$UART_ENABLE" != "true" ] && \
    config_error 'log_mode=trigger requires uart_enable=true (use log_mode=continuous otherwise)'
if [ -n "$LOG_DIR" ]; then
    case "$LOG_DIR" in /*) ;; *) config_error 'log_dir must be an absolute path';; esac
fi
# ---- 诊断开关 ----
for _k in print_enable print_boot print_isp print_target print_perf print_uart print_mcu print_log print_avi; do
    eval "_v=\${$(echo "$_k" | tr 'a-z' 'A-Z')}"
    case "$_v" in true|false) ;; *) config_error "$_k must be true or false";; esac
done
if [ -n "$STATE_DIR" ]; then
    case "$STATE_DIR" in /*) ;; *) config_error 'state_dir must be an absolute path';; esac
fi
validate_integer manual_exposure_us "$MANUAL_EXP_US" 100 10000
validate_integer manual_analog_gain_x1024 "$MANUAL_AGAIN" 1024 16384
validate_integer manual_digital_gain_x1024 "$MANUAL_DGAIN" 1024 4096
validate_integer manual_isp_gain_x1024 "$MANUAL_ISPGAIN" 1024 4096
validate_integer auto_exposure_max_us "$AUTO_EXP_MAX_US" 0 10000
validate_auto_gain auto_analog_gain_max_x1024 "$AUTO_AGAIN_MAX" 16384
validate_auto_gain auto_digital_gain_max_x1024 "$AUTO_DGAIN_MAX" 4096
validate_auto_gain auto_isp_gain_max_x1024 "$AUTO_ISPGAIN_MAX" 4096
validate_integer black_level_r "$BLC_R" 0 4095
validate_integer black_level_gr "$BLC_GR" 0 4095
validate_integer black_level_gb "$BLC_GB" 0 4095
validate_integer black_level_b "$BLC_B" 0 4095
validate_integer jpeg_quality "$JPEG_QUALITY" 75 90
[ "$RUN_SECONDS" -eq 0 ] || validate_integer run_seconds "$RUN_SECONDS" 20 300

DURATION=${1:-$RUN_SECONDS}
if [ "$DURATION" = '--check-config' ]; then
    echo "CONFIG_OK path=$CONFIG exposure_mode=$EXPOSURE_MODE manual_exp_us=$MANUAL_EXP_US gains=$MANUAL_AGAIN,$MANUAL_DGAIN,$MANUAL_ISPGAIN auto_limits=$AUTO_EXP_MAX_US,$AUTO_AGAIN_MAX,$AUTO_DGAIN_MAX,$AUTO_ISPGAIN_MAX blc=$BLC_MODE:$BLC_R,$BLC_GR,$BLC_GB,$BLC_B jpeg=$JPEG_QUALITY run_seconds=$RUN_SECONDS record_enable=$RECORD_ENABLE display_enable=$DISPLAY_ENABLE"
echo "CONFIG_TRACK track=$TRACK_ENABLE calib=$CAM_FX,$CAM_FY,$CAM_CX,$CAM_CY optical=$OPTICAL_YAW_DEG,$OPTICAL_PITCH_DEG invert=$CAM_INVERT_YAW,$CAM_INVERT_PITCH dia=$TARGET_DIAMETER_M aimup=$TARGET_UP_OFFSET_FACTOR sel=w$SEL_CONF_WEIGHT,a$SEL_MAX_ASPECT,c$SEL_CONFIRM_FRAMES,l$SEL_LOST_FRAMES,n$SEL_MAX_CANDIDATES kf=q$KF_PROCESS_NOISE,r$KF_MEASURE_NOISE uart=$UART_ENABLE,$UART_DEVICE@$UART_BAUD,$UART_RATE_HZ,fmt=$UART_FORMAT,rx=$UART_RX_PRINT@$UART_RX_PRINT_HZ,tx=$UART_TX_ENABLE osd=$OSD_ENABLE@x$OSD_SCALE"
echo "CONFIG_LOG enable=$LOG_ENABLE mode=$LOG_MODE dir=${LOG_DIR:-<root>/logs} preroll=$LOG_PREROLL_MS postroll=$LOG_POSTROLL_MS max=$LOG_MAX_SECONDS"
echo "CONFIG_DIAG state_dir=${STATE_DIR:-<root>/state} print=$PRINT_ENABLE boot=$PRINT_BOOT isp=$PRINT_ISP target=$PRINT_TARGET perf=$PRINT_PERF uart=$PRINT_UART mcu=$PRINT_MCU log=$PRINT_LOG avi=$PRINT_AVI"
    exit 0
fi
if [ "$DURATION" = '--recoding' ] || [ "$DURATION" = '--recording' ]; then
    RECORDING=1
    DURATION=${2:-$RUN_SECONDS}
fi
if [ "$RECORD_ENABLE" = 'true' ]; then
    RECORDING=1
fi
[ "$DURATION" -eq 0 ] || validate_integer duration "$DURATION" 20 300

# Exactly the values validated above are passed to the C++ ISP/VENC controls.
export RECORD80_EXPOSURE_MODE="$EXPOSURE_MODE"
export RECORD80_MANUAL_EXP_US="$MANUAL_EXP_US"
export RECORD80_MANUAL_AGAIN="$MANUAL_AGAIN"
export RECORD80_MANUAL_DGAIN="$MANUAL_DGAIN"
export RECORD80_MANUAL_ISPGAIN="$MANUAL_ISPGAIN"
export RECORD80_AUTO_EXP_MAX_US="$AUTO_EXP_MAX_US"
export RECORD80_AUTO_AGAIN_MAX="$AUTO_AGAIN_MAX"
export RECORD80_AUTO_DGAIN_MAX="$AUTO_DGAIN_MAX"
export RECORD80_AUTO_ISPGAIN_MAX="$AUTO_ISPGAIN_MAX"
export RECORD80_BLC_MODE="$BLC_MODE"
export RECORD80_BLC_R="$BLC_R"
export RECORD80_BLC_GR="$BLC_GR"
export RECORD80_BLC_GB="$BLC_GB"
export RECORD80_BLC_B="$BLC_B"
export RECORD80_JPEG_QUALITY="$JPEG_QUALITY"
echo "RUN_CONFIG mode=$EXPOSURE_MODE exposure_us=$MANUAL_EXP_US gain=$MANUAL_AGAIN,$MANUAL_DGAIN,$MANUAL_ISPGAIN blc=$BLC_MODE jpeg=$JPEG_QUALITY duration=$DURATION recording=$RECORDING record_enable=$RECORD_ENABLE display_enable=$DISPLAY_ENABLE http_enable=$HTTP_ENABLE"
REG=0x030028c4
ORIG=0x00200001
TARGET=0x00210001
LOCK=/code/fly_LicheeRV/.camera.lock
LOG=/tmp/fly_licheerv_capture.log
CAM_PID=''
GUARD_PID=''
OWN_LOCK=0
CHANGED=0
clock() { devmem "$REG" 32; }
finish() {
    code=$?
    trap - EXIT INT TERM HUP
    if [ -n "$CAM_PID" ]; then
        kill -TERM "$CAM_PID" 2>/dev/null || :
        wait "$CAM_PID" 2>/dev/null || :
    fi
    if [ "$CHANGED" -eq 1 ]; then
        current=$(clock || echo ERROR)
        if [ "$current" = "$TARGET" ]; then
            devmem "$REG" 32 "$ORIG" || echo CLOCK_RESTORE_FAILED
        elif [ "$current" != "$ORIG" ]; then
            echo "UNEXPECTED_CLOCK=$current, manual inspection required"
        fi
    fi
    if [ -n "$GUARD_PID" ]; then kill "$GUARD_PID" 2>/dev/null || :; fi
    if [ "$OWN_LOCK" -eq 1 ]; then
        if [ -f "$LOCK/supervisor.pid" ] && [ "$(cat "$LOCK/supervisor.pid")" = "$$" ]; then
            rm -f "$LOCK/supervisor.pid" "$LOCK/camera.pid" "$LOCK/guard.pid"
            rmdir "$LOCK" || echo LOCK_RELEASE_FAILED
        fi
    fi
    if [ "$OWN_LOCK" -eq 1 ]; then
        echo "FINAL_CLOCK=$(clock)"
        echo "LOG=$LOG"
    else
        echo 'NO_CAMERA_TOUCHED: existing session was left running'
    fi
    exit "$code"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
[ -x "$ROOT/camera" ] || { echo 'Missing /code/fly_LicheeRV/camera'; exit 3; }
[ "$(uname -r)" = '5.10.4-tag-' ] || { echo WRONG_KERNEL; exit 3; }
if [ -e "$LOCK" ]; then
    echo 'CHECKING_PREVIOUS_LOCK'
    if ! sh "$ROOT/stop.sh" --recover; then
        echo 'CAMERA_IN_USE; to stop it: sh /code/fly_LicheeRV/stop.sh'
        exit 3
    fi
fi
if pidof camera camera_pure80 camera_36record camera_capture_first_320 camera_http80 camera_preview80 camera_live80 sample_vio >/dev/null 2>&1; then
    echo 'CAMERA_IN_USE; to stop: sh /code/fly_LicheeRV/stop.sh'
    exit 3
fi
TEMP=$(cat /sys/class/thermal/thermal_zone0/temp)
[ "$TEMP" -lt 70000 ] || { echo "TOO_HOT_TO_START=$TEMP"; exit 4; }
[ "$(clock)" = "$ORIG" ] || { echo CLOCK_NOT_ORIGINAL; exit 4; }
PARENT=$(cat /sys/kernel/debug/clk/clk_cam0pll/clk_rate)
[ "$PARENT" -gt 1187000000 ] && [ "$PARENT" -lt 1189000000 ] || { echo WRONG_PARENT_CLOCK; exit 4; }
mkdir "$LOCK"
OWN_LOCK=1
printf '%s\n' "$$" > "$LOCK/supervisor.pid"
export LD_LIBRARY_PATH=/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/usr/bin/lib
# Listen on actual Wi-Fi IP; fallback USB NCM so browsers on either link can preview.
WEB_IP=$(ifconfig wlan0 2>/dev/null | awk '/inet addr:/ {sub(/^addr:/, "", $2); print $2; exit}')
[ -n "$WEB_IP" ] || WEB_IP=10.133.124.1
case "$WEB_BIND" in
    auto) export WEB_IP_BIND=auto; WEB_IP=0.0.0.0;;
    usb)  export WEB_IP_BIND=10.133.124.1; WEB_IP=10.133.124.1;;
    wifi) export WEB_IP_BIND=$(ifconfig wlan0 2>/dev/null | awk '/inet addr:/ {sub(/^addr:/, "", $2); print $2; exit}'); WEB_IP=$WEB_IP_BIND;;
esac
export DISPLAY_ENABLE="$DISPLAY_ENABLE"
export HTTP_ENABLE="$HTTP_ENABLE"
export DETECT_SAMPLE_STEPS="$DETECT_SAMPLE_STEPS"
export DETECT_STRIDE="$DETECT_STRIDE"
export DETECT_ENABLE="$DETECT_ENABLE"
export OSD_ENABLE="$OSD_ENABLE"
export OSD_SCALE="$OSD_SCALE"
# ---- 阶段1：目标跟踪 / 标定 / 串口（camera.cpp 里严格校验范围）----
export TRACK_ENABLE="$TRACK_ENABLE"
export CAM_FX="$CAM_FX"
export CAM_FY="$CAM_FY"
export CAM_CX="$CAM_CX"
export CAM_CY="$CAM_CY"
export OPTICAL_YAW_DEG="$OPTICAL_YAW_DEG"
export OPTICAL_PITCH_DEG="$OPTICAL_PITCH_DEG"
export CAM_INVERT_YAW="$CAM_INVERT_YAW"
export CAM_INVERT_PITCH="$CAM_INVERT_PITCH"
export TARGET_DIAMETER_M="$TARGET_DIAMETER_M"
export TARGET_UP_OFFSET_FACTOR="$TARGET_UP_OFFSET_FACTOR"
export SEL_CONF_WEIGHT="$SEL_CONF_WEIGHT"
export SEL_MAX_ASPECT="$SEL_MAX_ASPECT"
export SEL_MAX_DIST_DEG="$SEL_MAX_DIST_DEG"
export SEL_MAX_CANDIDATES="$SEL_MAX_CANDIDATES"
export SEL_CONFIRM_FRAMES="$SEL_CONFIRM_FRAMES"
export SEL_LOST_FRAMES="$SEL_LOST_FRAMES"
export KF_PROCESS_NOISE="$KF_PROCESS_NOISE"
export KF_MEASURE_NOISE="$KF_MEASURE_NOISE"
export KF_INIT_POS_VAR="$KF_INIT_POS_VAR"
export KF_INIT_VEL_VAR="$KF_INIT_VEL_VAR"
export UART_ENABLE="$UART_ENABLE"
export UART_DEVICE="$UART_DEVICE"
export UART_BAUD="$UART_BAUD"
export UART_RATE_HZ="$UART_RATE_HZ"
export UART_FORMAT="$UART_FORMAT"
export UART_RX_PRINT="$UART_RX_PRINT"
export UART_RX_PRINT_HZ="$UART_RX_PRINT_HZ"
export UART_TX_ENABLE="$UART_TX_ENABLE"
# 记录目录：配置里留空就用 $ROOT/logs（跟二进制/配置在一起，便于 scp 取回）
# 一次飞行 = record/ 下的一个文件夹（视频 + CSV + meta 都在一起）
[ -n "$LOG_DIR" ] || LOG_DIR="$ROOT/record"
[ "$LOG_ENABLE" = "true" ] && mkdir -p "$LOG_DIR"
export LOG_ENABLE="$LOG_ENABLE"
export LOG_MODE="$LOG_MODE"
export LOG_DIR="$LOG_DIR"
export LOG_PREROLL_MS="$LOG_PREROLL_MS"
export LOG_POSTROLL_MS="$LOG_POSTROLL_MS"
export LOG_MAX_SECONDS="$LOG_MAX_SECONDS"
export LOG_MCU_STREAM="$LOG_MCU_STREAM"

# ---- 运行状态历史（自启动时唯一的事后可见通道）----
# 与 camera 进程写同一个文件：一行一件事，跨重启保留（>256KB 由 camera 自己轮转）
[ -n "$STATE_DIR" ] || STATE_DIR="$ROOT/state"
mkdir -p "$STATE_DIR" 2>/dev/null || STATE_DIR=""
CAM_STARTED=0
EXIT_REASON=normal
note() {
    # note <tag> <message>
    [ -n "$STATE_DIR" ] || return 0
    _t=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)
    _w=$(date -u +%Y-%m-%dT%H:%M:%S 2>/dev/null || echo unknown)
    printf 't=%ss wall=%s pid=%s tag=%s %s\n' "$_t" "$_w" "$$" "$1" "$2" >>"$STATE_DIR/run_history.log" 2>/dev/null || true
    return 0
}
on_exit() {
    _rc=$?
    [ "$CAM_STARTED" = "1" ] || return 0
    note supervisor_exit "reason=$EXIT_REASON rc=$_rc duration=${DURATION:-?}"
    return 0
}
trap on_exit EXIT
# ssh 断开/被 kill 时也要留下"我是怎么没的"——不然历史上只剩一条 supervisor_start
trap 'EXIT_REASON=hangup; exit 129' HUP
trap 'EXIT_REASON=terminated; exit 143' TERM
trap 'EXIT_REASON=interrupted; exit 130' INT
export STATE_DIR
export PRINT_ENABLE PRINT_BOOT PRINT_ISP PRINT_TARGET PRINT_PERF PRINT_UART PRINT_MCU PRINT_LOG PRINT_AVI
note supervisor_start "config=$CONFIG duration=${DURATION:-?} display=$DISPLAY_ENABLE record=$RECORDING uart=$UART_ENABLE/$UART_TX_ENABLE log=$LOG_ENABLE/$LOG_MODE print=$PRINT_ENABLE"
if [ "$RECORDING" -eq 1 ]; then
    RECORD_DIR="$ROOT/record"
    mkdir -p "$RECORD_DIR"
    "$ROOT/camera" "$WEB_IP" "$RECORD_DIR" 80 --recording-internal >"$LOG" 2>&1 &
else
    "$ROOT/camera" "$WEB_IP" /tmp 80 >"$LOG" 2>&1 &
fi
CAM_PID=$!
printf '%s\n' "$CAM_PID" > "$LOCK/camera.pid"
sleep 7
kill -0 "$CAM_PID" 2>/dev/null || { echo CAMERA_EXITED_EARLY; tail -15 "$LOG"; exit 5; }
grep -q '^CAMERA_READY' "$LOG" || { echo CAMERA_NOT_READY; tail -15 "$LOG"; exit 5; }
if [ "$EXPOSURE_MODE" = manual ]; then
    grep -q "^EXPOSURE_SET us=$MANUAL_EXP_US .*rc=0$" "$LOG" || { echo EXPOSURE_NOT_APPLIED; tail -15 "$LOG"; exit 5; }
else
    grep -q '^RECORD80_AE_AUTO_LIMIT .*rc=0x0$' "$LOG" || { echo AUTO_EXPOSURE_NOT_APPLIED; tail -15 "$LOG"; exit 5; }
fi
ERROR_BEFORE=$(awk '/^[ ]*0[ ]+[0-9]+[ ]+[0-9]+[ ]+[0-9]+/{print $2,$3,$4,$5,$6;exit}' /proc/mipi-rx)
# If the supervisor is forcibly killed, recover its camera and clock at the deadline.
# Ownership checks prevent an old guard from killing a new session or touching its clock.
# With run_seconds=0 (always-on) there is no deadline, so no guard is spawned.
if [ "$DURATION" -gt 0 ]; then
(
    sleep $((DURATION + 20))
    if [ -r "$LOCK/supervisor.pid" ] && [ "$(cat "$LOCK/supervisor.pid")" = "$$" ] &&
       [ -r "$LOCK/camera.pid" ] && [ "$(cat "$LOCK/camera.pid")" = "$CAM_PID" ]; then
        if [ -r "/proc/$CAM_PID/cmdline" ] &&
           [ "$(tr '\000' '\n' < "/proc/$CAM_PID/cmdline" | head -n 1)" = "$ROOT/camera" ]; then
            kill -TERM "$CAM_PID" 2>/dev/null || :
            sleep 3
        fi
        if ! kill -0 "$CAM_PID" 2>/dev/null &&
           ! pidof camera camera_pure80 camera_36record camera_capture_first_320 camera_http80 camera_preview80 camera_live80 sample_vio >/dev/null 2>&1; then
            [ "$(clock)" != "$TARGET" ] || devmem "$REG" 32 "$ORIG" >/dev/null 2>&1 || :
            rm -f "$LOCK/supervisor.pid" "$LOCK/camera.pid" "$LOCK/guard.pid"
            rmdir "$LOCK" 2>/dev/null || :
        fi
    fi
) &
GUARD_PID=$!
printf '%s\n' "$GUARD_PID" > "$LOCK/guard.pid"
fi
CHANGED=1
devmem "$REG" 32 "$TARGET"
[ "$(clock)" = "$TARGET" ] || { echo CLOCK_SWITCH_FAILED; exit 5; }
echo "CAMERA_RUNNING fps_target=80 exposure_mode=$EXPOSURE_MODE manual_exposure_us=$MANUAL_EXP_US jpeg_quality=$JPEG_QUALITY duration=${DURATION}s"
# 这里只说「武装/关闭」，不要说「正在录」：真正开录要等电控的起飞边沿（log_mode: trigger）
if [ "$RECORDING" -eq 1 ]; then
    echo "RECORDING_ARMED output_dir=$RECORD_DIR format=AVI/MJPEG (等待起飞触发才写文件)"
else
    echo "RECORDING_OFF (record_enable=false)"
fi
if [ "$HTTP_ENABLE" = "true" ] && [ "$DISPLAY_ENABLE" = "true" ]; then
    echo "WEB_READY http://$WEB_IP:8081/"
else
    echo "WEB_DISABLED (http_enable=$HTTP_ENABLE display_enable=$DISPLAY_ENABLE)"
fi
CAM_STARTED=1
note ready "web=$([ "$HTTP_ENABLE" = "true" ] && [ "$DISPLAY_ENABLE" = "true" ] && echo "$WEB_IP" || echo off) cam_pid=$CAM_PID fps_target=80"
START=$(date +%s)
LAST_STATS=''
while [ "$DURATION" -eq 0 ] || [ $(( $(date +%s) - START )) -lt "$DURATION" ]; do
    sleep 3
    kill -0 "$CAM_PID" 2>/dev/null || { EXIT_REASON=camera_crash; echo CAMERA_CRASH; tail -20 "$LOG"; exit 6; }
    TEMP=$(cat /sys/class/thermal/thermal_zone0/temp)
    [ "$TEMP" -lt 74500 ] || { EXIT_REASON=temp_limit; note temp_limit "temp_millic=$TEMP limit_millic=74500"; echo "TEMP_LIMIT=$TEMP"; exit 7; }
    [ "$(clock)" = "$TARGET" ] || { EXIT_REASON=clock_changed; echo CLOCK_CHANGED; exit 8; }
    ERROR_NOW=$(awk '/^[ ]*0[ ]+[0-9]+[ ]+[0-9]+[ ]+[0-9]+/{print $2,$3,$4,$5,$6;exit}' /proc/mipi-rx)
    [ "$ERROR_NOW" = "$ERROR_BEFORE" ] || { EXIT_REASON=mipi_changed; echo "MIPI_CHANGED before=$ERROR_BEFORE after=$ERROR_NOW"; exit 9; }
    NEW_STATS=$(grep '^capture_frames=' "$LOG" | tail -n 1 || :)
    if [ -n "$NEW_STATS" ] && [ "$NEW_STATS" != "$LAST_STATS" ]; then
        echo "$NEW_STATS"
        LAST_STATS=$NEW_STATS
    fi
done
if [ "$DURATION" -eq 0 ]; then EXIT_REASON=run_ended; echo RUN_ENDED_UNEXPECTEDLY; else EXIT_REASON=test_completed; echo TEST_COMPLETED; fi
