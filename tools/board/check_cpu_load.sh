#!/bin/sh
# 量单核负载：整机 CPU 占用 + 各线程占用 + 我们的进程占比
echo "=== 整机 CPU（2 秒窗口，/proc/stat）==="
read_cpu() { awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8}' /proc/stat; }
A=$(read_cpu); sleep 2; B=$(read_cpu)
echo "$A $B" | awk '{
  a1=$1;a2=$2;a3=$3;a4=$4;a5=$5;a6=$6;a7=$7;
  b1=$8;b2=$9;b3=$10;b4=$11;b5=$12;b6=$13;b7=$14;
  idle_a=a4+a5; idle_b=b4+b5;
  tot_a=a1+a2+a3+a4+a5+a6+a7; tot_b=b1+b2+b3+b4+b5+b6+b7;
  dt=tot_b-tot_a; di=idle_b-idle_a;
  printf "  总 CPU 占用 = %.1f%%  (窗口内 jiffies %d)\n", (dt-di)*100.0/dt, dt;
}'

echo
echo "=== 进程 CPU 占用（top 前 6，单位 %）==="
top -b -n 2 -d 1 2>/dev/null | awk '/^ *PID/,0' | tail -8 | head -7

echo
echo "=== camera 各线程占用（/proc/<pid>/stat 差分 1 秒）==="
PID=$(pidof camera 2>/dev/null | awk '{print $1}')
if [ -z "$PID" ]; then
    echo "  camera 没在跑"
else
    echo "  camera pid = $PID"
    for t in /proc/$PID/task/*; do
        tid=$(basename "$t")
        n=$(cat "$t/comm" 2>/dev/null)
        # utime(14) stime(15) 在 stat 里，跳过 comm 的括号
        s=$(awk '{print $14, $15}' "$t/stat" 2>/dev/null)
        echo "$tid $n $s"
    done > /tmp/t0.txt
    sleep 1
    for t in /proc/$PID/task/*; do
        tid=$(basename "$t")
        s=$(awk '{print $14, $15}' "$t/stat" 2>/dev/null)
        echo "$tid $s"
    done > /tmp/t1.txt
    HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
    awk -v hz="$HZ" 'NR==FNR{u1[$1]=$3; s1[$1]=$4; next}
      {du=$3-u1[$1]; ds=$4-s1[$1];
       printf "  tid=%-6s %-16s CPU=%5.1f%%\n", $1, "", (du+ds)*100.0/hz}' /tmp/t0.txt /tmp/t1.txt \
      | sort -t= -k4 -rn | head -8
    echo
    echo "  线程名对照："
    awk '{printf "    %-6s %s\n", $1, $2}' /tmp/t0.txt
fi

echo
echo "=== 内存 ==="
head -4 /proc/meminfo
echo
echo "=== 温度 ==="
awk '{printf "  %.1f C\n", $1/1000}' /sys/class/thermal/thermal_zone0/temp
