#!/bin/sh
# Standalone GC4653 / 80 FPS / 3000us; optional native MJPEG AVI recording.
set -eu
ROOT=/code/fly_LicheeRV
CONFIG=${FLY_CONFIG_FILE:-$ROOT/config.yaml}

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
        *) config_error "unknown key $key";;
    esac
done < "$CONFIG"

case "$EXPOSURE_MODE" in manual|auto) ;; *) config_error 'exposure_mode must be manual or auto';; esac
case "$RECORD_ENABLE" in true|false) ;; *) config_error 'record_enable must be true or false';; esac
case "$WEB_BIND" in auto|usb|wifi) ;; *) config_error 'web_ip must be auto, usb or wifi';; esac
case "$BLC_MODE" in manual|default) ;; *) config_error 'black_level_mode must be manual or default';; esac
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
validate_integer run_seconds "$RUN_SECONDS" 20 300

DURATION=${1:-$RUN_SECONDS}
if [ "$DURATION" = '--check-config' ]; then
    echo "CONFIG_OK path=$CONFIG exposure_mode=$EXPOSURE_MODE manual_exp_us=$MANUAL_EXP_US gains=$MANUAL_AGAIN,$MANUAL_DGAIN,$MANUAL_ISPGAIN auto_limits=$AUTO_EXP_MAX_US,$AUTO_AGAIN_MAX,$AUTO_DGAIN_MAX,$AUTO_ISPGAIN_MAX blc=$BLC_MODE:$BLC_R,$BLC_GR,$BLC_GB,$BLC_B jpeg=$JPEG_QUALITY run_seconds=$RUN_SECONDS record_enable=$RECORD_ENABLE"
    exit 0
fi
if [ "$DURATION" = '--recoding' ] || [ "$DURATION" = '--recording' ]; then
    RECORDING=1
    DURATION=${2:-$RUN_SECONDS}
fi
if [ "$RECORD_ENABLE" = 'true' ]; then
    RECORDING=1
fi
validate_integer duration "$DURATION" 20 300

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
echo "RUN_CONFIG mode=$EXPOSURE_MODE exposure_us=$MANUAL_EXP_US gain=$MANUAL_AGAIN,$MANUAL_DGAIN,$MANUAL_ISPGAIN blc=$BLC_MODE jpeg=$JPEG_QUALITY duration=$DURATION recording=$RECORDING record_enable=$RECORD_ENABLE"
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
export PREVIEW_DIAG_LINEAR=1
if [ "$RECORDING" -eq 1 ]; then
    RECORD_DIR="$ROOT/recordings"
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
CHANGED=1
devmem "$REG" 32 "$TARGET"
[ "$(clock)" = "$TARGET" ] || { echo CLOCK_SWITCH_FAILED; exit 5; }
echo "CAMERA_RUNNING fps_target=80 exposure_mode=$EXPOSURE_MODE manual_exposure_us=$MANUAL_EXP_US jpeg_quality=$JPEG_QUALITY duration=${DURATION}s"
if [ "$RECORDING" -eq 1 ]; then
    echo "RECORDING_MODE=ON output_dir=$RECORD_DIR format=AVI/MJPEG"
fi
echo "WEB_READY http://$WEB_IP:8081/"
START=$(date +%s)
LAST_STATS=''
while [ $(( $(date +%s) - START )) -lt "$DURATION" ]; do
    sleep 3
    kill -0 "$CAM_PID" 2>/dev/null || { echo CAMERA_CRASH; tail -20 "$LOG"; exit 6; }
    TEMP=$(cat /sys/class/thermal/thermal_zone0/temp)
    [ "$TEMP" -lt 74500 ] || { echo "TEMP_LIMIT=$TEMP"; exit 7; }
    [ "$(clock)" = "$TARGET" ] || { echo CLOCK_CHANGED; exit 8; }
    ERROR_NOW=$(awk '/^[ ]*0[ ]+[0-9]+[ ]+[0-9]+[ ]+[0-9]+/{print $2,$3,$4,$5,$6;exit}' /proc/mipi-rx)
    [ "$ERROR_NOW" = "$ERROR_BEFORE" ] || { echo "MIPI_CHANGED before=$ERROR_BEFORE after=$ERROR_NOW"; exit 9; }
    NEW_STATS=$(grep '^capture_frames=' "$LOG" | tail -n 1 || :)
    if [ -n "$NEW_STATS" ] && [ "$NEW_STATS" != "$LAST_STATS" ]; then
        echo "$NEW_STATS"
        LAST_STATS=$NEW_STATS
    fi
done
echo TEST_COMPLETED
