#!/bin/sh
# 调查：第二个核能否被 Linux 用起来
echo "=== 1) 内核是否编译了 SMP ==="
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz 2>/dev/null | grep -E '^CONFIG_SMP|^CONFIG_NR_CPUS|^CONFIG_RISCV_SBI|^CONFIG_RISCV_M_MODE'
elif [ -r /boot/config-$(uname -r) ]; then
    grep -E '^CONFIG_SMP|^CONFIG_NR_CPUS|^CONFIG_RISCV_SBI' /boot/config-$(uname -r)
else
    echo "  (/proc/config.gz 与 /boot/config-* 都没有)"
    echo -n "  kallsyms 里 smp 相关符号数: "
    grep -ciE 'smp_init|smp_setup|__cpu_up|smp_call_function' /proc/kallsyms 2>/dev/null
    grep -E ' (smp_init|smp_setup_processor_id|__cpu_up|smp_send_reschedule)$' /proc/kallsyms 2>/dev/null | head -4
fi
echo
echo "=== 2) 设备树 cpus 节点全貌 ==="
ls -1 /proc/device-tree/cpus/ 2>/dev/null | sed 's/^/  /'
echo "  cpu@0 reg = $(od -An -tx1 /proc/device-tree/cpus/cpu@0/reg 2>/dev/null | tr -s ' ')"
for c in /proc/device-tree/cpus/cpu@*; do
    [ -d "$c" ] || continue
    echo "  --- $(basename $c) ---"
    echo "    clock-frequency = $(od -An -tu4 --endian=big "$c/clock-frequency" 2>/dev/null | tr -s ' ')"
    echo "    status          = $(tr -d '\000' < "$c/status" 2>/dev/null)"
    echo "    enable-method   = $(tr -d '\000' < "$c/enable-method" 2>/dev/null)"
done
echo
echo "=== 3) 有没有 remoteproc / rpmsg / mailbox 驱动 ==="
echo -n "  /dev: "; ls /dev 2>/dev/null | grep -iE 'rpmsg|remoteproc|mailbox' || echo "(无)"
echo -n "  /sys/class/remoteproc: "; ls /sys/class/remoteproc 2>/dev/null || echo "(无)"
echo -n "  kallsyms rpmsg/remoteproc 符号数: "; grep -ciE 'rpmsg|remoteproc' /proc/kallsyms 2>/dev/null
echo
echo "=== 4) 启动参数 / 内核命令行 ==="
cat /proc/cmdline 2>/dev/null | sed 's/^/  /'
echo
echo "=== 5) dmesg 里 CPU / SMP 相关 ==="
dmesg 2>/dev/null | grep -iE 'smp|cpu[0-9]|hart|riscv' | head -10
echo
echo "=== 6) bootloader 是否提到第二核 ==="
ls -l /boot 2>/dev/null | head -8
