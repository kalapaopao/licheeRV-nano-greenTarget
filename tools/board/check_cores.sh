#!/bin/sh
# 查这块板子 Linux 实际能用几个核
echo "=== nproc / online ===实时"
echo "  nproc        = $(nproc 2>/dev/null)"
echo "  cpuinfo 核数 = $(grep -c '^processor' /proc/cpuinfo 2>/dev/null)"
echo "  online       = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
echo "  possible     = $(cat /sys/devices/system/cpu/possible 2>/dev/null)"
echo "  present      = $(cat /sys/devices/system/cpu/present 2>/dev/null)"
echo
echo "=== /proc/cpuinfo 摘要 ==="
grep -E '^(processor|hart|model name|isa|mmu|uarch)' /proc/cpuinfo 2>/dev/null | head -20
echo
echo "=== 小核 / 双核相关痕迹 ==="
echo -n "  dmesg 里 c906l/remoteproc/rpmsg: "
dmesg 2>/dev/null | grep -ciE 'c906l|remoteproc|rpmsg|mailbox|small.?core' || echo 0
dmesg 2>/dev/null | grep -iE 'c906l|remoteproc|rpmsg|mailbox' | head -5
echo -n "  /dev 里 remoteproc/rpmsg: "
ls /dev 2>/dev/null | grep -iE 'rpmsg|remoteproc' || echo "(无)"
echo
echo "=== 设备树里的 cpu 节点 ==="
for d in /proc/device-tree/cpus/cpu@*; do
    [ -d "$d" ] || continue
    echo "  $(basename $d):"
    for f in device_type compatible reg clock-frequency status enable-method; do
        [ -f "$d/$f" ] && printf '    %-16s %s\n' "$f" "$(tr -d '\000' < "$d/$f" 2>/dev/null | head -c 80)"
    done
done
echo
echo "=== 当前负载 ==="
uptime 2>/dev/null
cat /proc/loadavg 2>/dev/null
