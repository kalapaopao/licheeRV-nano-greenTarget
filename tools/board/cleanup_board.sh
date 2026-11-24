#!/bin/sh
# 清理板上测试残留：杀 camera/start.sh、停服务、清空 record
cd /code/fly_LicheeRV || exit 1
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in
        *fly_LicheeRV/camera*|*fly_LicheeRV/start.sh*|*fake_flight*)
            echo "  kill $pid : $(echo "$c" | cut -c1-60)"
            kill -9 "$pid" 2>/dev/null
            ;;
    esac
done
sleep 1
sh stop.sh >/dev/null 2>&1
sleep 1
rm -rf record/sess_*
echo "剩余 camera/start.sh 进程数: $(ps | grep -v grep | grep -cE 'camera|start\.sh')"
echo "record 目录条目数: $(ls -1 record/ 2>/dev/null | wc -l)"
echo "温度: $(awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp) C"
echo "磁盘: $(df -h /dev/mmcblk0p2 | tail -1)"
