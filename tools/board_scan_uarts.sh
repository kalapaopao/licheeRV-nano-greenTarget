#!/bin/sh
# 电控正在发：扫一遍所有 UART，看字节到底落在哪个口上
set -u
sh /code/fly_LicheeRV/stop.sh >/dev/null 2>&1 || true
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    if grep -q 'fly_LicheeRV/camera' "$p/cmdline" 2>/dev/null; then kill "$pid" 2>/dev/null; fi
done
sleep 2

echo "=== 各 UART 计数（rx 是内核收到的字节数）==="
cat /proc/tty/driver/serial
echo

for N in 0 1 2 3 4; do
    DEV=/dev/ttyS$N
    [ -c "$DEV" ] || continue
    BEFORE=$(grep -E "^$N: " /proc/tty/driver/serial | sed -n 's/.*rx:\([0-9]*\).*/\1/p')
    stty -F $DEV 115200 cs8 -cstopb -parenb raw -echo -crtscts -ixon -ixoff 2>/dev/null
    dd if=$DEV of=/tmp/rx$N.bin bs=1 count=200 2>/dev/null &
    DDPID=$!
    sleep 2
    kill $DDPID 2>/dev/null
    wait $DDPID 2>/dev/null
    AFTER=$(grep -E "^$N: " /proc/tty/driver/serial | sed -n 's/.*rx:\([0-9]*\).*/\1/p')
    SIZE=$(wc -c < /tmp/rx$N.bin 2>/dev/null || echo 0)
    echo "ttyS$N: 内核 rx $BEFORE -> $AFTER (增 $((AFTER - BEFORE)))  抓包 $SIZE 字节"
    if [ "$SIZE" -gt 0 ]; then
        od -An -tx1 -v /tmp/rx$N.bin | head -3 | sed 's/^/    /'
    fi
done
