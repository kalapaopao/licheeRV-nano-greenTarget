#!/bin/sh
# 数一数 camera 进程到底有几个线程，各自在干什么
cd /code/fly_LicheeRV || exit 1
P=$(pidof camera)
[ -n "$P" ] || { echo "camera 未运行"; exit 1; }
echo "pid=$P"
echo "Threads: $(grep Threads /proc/$P/status)"
echo
echo "=== 每个线程：名字 / 状态 / 累计 CPU 时间(ticks) / 当前内核等待点 ==="
for t in /proc/$P/task/*; do
    tid=$(basename "$t")
    comm=$(cat "$t/comm" 2>/dev/null)
    st=$(awk '{print $3}' "$t/stat" 2>/dev/null)
    ut=$(awk '{print $14}' "$t/stat" 2>/dev/null)
    wchan=$(cat "$t/wchan" 2>/dev/null)
    [ -n "$wchan" ] || wchan="-"
    printf "  tid=%-6s comm=%-16s state=%s utime=%-5s wchan=%s\n" "$tid" "$comm" "$st" "$ut" "$wchan"
done
echo
echo "=== 主线程（tgid=$P）的 CPU 与上下文切换 ==="
grep -E 'voluntary_ctxt_switches|nonvoluntary_ctxt_switches' /proc/$P/status
