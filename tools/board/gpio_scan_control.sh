#!/bin/sh
# 清掉遗留的 console 狂写进程，然后做三次对照采样
cd /code/fly_LicheeRV || exit 1

echo "=== 清理遗留的 ttyS0 写进程 ==="
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in
        *"/dev/ttyS0"*) echo "  kill $pid: $c"; kill -9 "$pid" 2>/dev/null ;;
    esac
done
sleep 1

echo
echo "=== ① 完全静默 2 秒（相机停、console 不写）==="
python3 /tmp/gpio_scan.py 2

echo
echo "=== ② 对照：故意往 console 写，验证采样法有效 ==="
( i=0; while [ $i -lt 200 ]; do printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789' > /dev/ttyS0 2>/dev/null; i=$((i+1)); done ) &
SP=$!
python3 /tmp/gpio_scan.py 2
kill $SP 2>/dev/null
wait $SP 2>/dev/null
sleep 1

echo
echo "=== ③ 再静默一次，确认 console 停了 ==="
python3 /tmp/gpio_scan.py 2

echo
echo "=== 内核 rx 计数（ttyS0~S3）==="
cat /proc/tty/driver/serial | head -5
