#!/bin/sh
# 电控正在发送时：直接抓 UART1 原始字节，回答「线上到底有没有数据、长什么样」
set -u
DEV=/dev/ttyS1

# 相机必须先停，否则字节被它读走
sh /code/fly_LicheeRV/stop.sh >/dev/null 2>&1 || true
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    if grep -q 'fly_LicheeRV/camera' "$p/cmdline" 2>/dev/null; then kill "$pid" 2>/dev/null; fi
done
sleep 2

rx_count() {
    grep -E '^1: ' /proc/tty/driver/serial 2>/dev/null | sed -n 's/.*rx:\([0-9]*\).*/\1/p'
}

echo "=== 把 ttyS1 设成 raw 115200 8N1（关软/硬流控）==="
stty -F $DEV 115200 cs8 -cstopb -parenb raw -echo -crtscts -ixon -ixoff 2>&1 | head -2
stty -F $DEV -a 2>/dev/null | head -1

BEFORE=$(rx_count)
echo
echo "=== 抓 3 秒原始字节（相机已停）==="
dd if=$DEV of=/tmp/rx_raw.bin bs=1 count=600 2>/dev/null &
DDPID=$!
sleep 3
kill $DDPID 2>/dev/null
wait $DDPID 2>/dev/null
AFTER=$(rx_count)

SIZE=$(wc -c < /tmp/rx_raw.bin 2>/dev/null || echo 0)
echo "内核 rx 计数：$BEFORE → $AFTER   （增量 $((AFTER - BEFORE)) 字节）"
echo "dd 抓到：$SIZE 字节"
echo
if [ "$SIZE" -gt 0 ]; then
    echo "=== 前 96 字节（十六进制）==="
    od -An -tx1 -v /tmp/rx_raw.bin | head -6
    echo
    echo "=== 前 96 字节（可见字符）==="
    head -c 96 /tmp/rx_raw.bin | od -An -c -v | head -4
else
    echo "!! 一个字节都没抓到"
fi
