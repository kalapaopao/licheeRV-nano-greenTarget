#!/bin/sh
# Safe stop and stale-lock recovery for this project's camera only.
set -eu
ROOT=/code/fly_LicheeRV
LOCK=$ROOT/.camera.lock
REG=0x030028c4
ORIG=0x00200001
TARGET=0x00210001
MODE=${1:-stop}
case "$MODE" in stop|--recover) ;; *) echo 'Usage: sh stop.sh [--recover]'; exit 2;; esac
camera_pids() { pidof camera 2>/dev/null || :; }
other_cameras() { pidof camera_pure80 camera_36record camera_capture_first_320 camera_http80 camera_preview80 camera_live80 sample_vio 2>/dev/null || :; }
is_our_camera() {
    [ -r "/proc/$1/cmdline" ] || return 1
    cmd=$(tr '\000' '\n' < "/proc/$1/cmdline" | head -n 1)
    [ "$cmd" = "$ROOT/camera" ]
}
is_our_supervisor() {
    [ -r "/proc/$1/cmdline" ] || return 1
    cmd=$(tr '\000' ' ' < "/proc/$1/cmdline")
    case "$cmd" in *"start.sh"*) return 0;; *) return 1;; esac
}
# Check any 'camera' process belongs to this project; never touch another app.
for p in $(camera_pids); do
    if ! is_our_camera "$p"; then echo "FOREIGN_CAMERA_ACTIVE pid=$p; refusing"; exit 3; fi
done
[ -z "$(other_cameras)" ] || { echo 'OTHER_CAMERA_ACTIVE: refusing cleanup'; exit 3; }
owner=''
if [ -r "$LOCK/supervisor.pid" ]; then owner=$(cat "$LOCK/supervisor.pid"); fi
case "$owner" in *[!0-9]* ) echo 'INVALID_OWNER_PID: refusing'; exit 3;; esac
if [ -n "$owner" ] && is_our_supervisor "$owner"; then
    if [ "$MODE" = --recover ]; then
        echo "CAMERA_ALREADY_RUNNING supervisor=$owner (use sh stop.sh to stop)"
        exit 3
    fi
    echo "STOPPING supervisor=$owner"
    kill -TERM "$owner" 2>/dev/null || :
    i=0
    while [ "$i" -lt 12 ] && [ -d "$LOCK" ]; do sleep 1; i=$((i+1)); done
fi
# Failed supervisor, orphaned camera, or older lock without PID: graceful stop.
for p in $(camera_pids); do
    if is_our_camera "$p"; then
        echo "STOP_ORPHAN_CAMERA pid=$p"
        kill -TERM "$p" 2>/dev/null || :
        i=0
        while [ "$i" -lt 10 ] && kill -0 "$p" 2>/dev/null; do sleep 1; i=$((i+1)); done
    fi
done
[ -z "$(camera_pids)" ] || { echo 'CAMERA_STILL_ACTIVE: no clock/lock cleanup'; exit 4; }
[ -z "$(other_cameras)" ] || { echo 'OTHER_CAMERA_ACTIVE: no clock/lock cleanup'; exit 4; }
current=$(devmem "$REG" 32)
if [ "$current" = "$TARGET" ]; then
    echo 'RESTORE_CLOCK'
    devmem "$REG" 32 "$ORIG"
elif [ "$current" != "$ORIG" ]; then
    echo "UNKNOWN_CLOCK=$current; manual inspection required"
    exit 5
fi
if [ -d "$LOCK" ]; then
    # Keep unknown file names: never rm -rf a directory just because it looks like a lock.
    for file in "$LOCK"/* "$LOCK"/.[!.]* "$LOCK"/..?*; do
        [ -e "$file" ] || continue
        case "$file" in "$LOCK/supervisor.pid"|"$LOCK/camera.pid"|"$LOCK/guard.pid") rm -f -- "$file";;
            *) echo "UNKNOWN_LOCK_CONTENT=$file; cannot remove"; exit 6;; esac
    done
    rmdir "$LOCK"
    echo STALE_LOCK_CLEARED
fi
[ "$(devmem "$REG" 32)" = "$ORIG" ] || exit 7
echo 'CAMERA_STOPPED clock=0x00200001 lock=free'
