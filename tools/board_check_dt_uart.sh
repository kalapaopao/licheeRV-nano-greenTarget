#!/bin/sh
# 查 UART1 的设备树与引脚复用状态（只读）
set -u
echo "=== 设备树 serial 节点 ==="
for d in /proc/device-tree/serial@*; do
    [ -d "$d" ] || continue
    echo "[$(basename "$d")]"
    for f in status compatible clock-frequency pinctrl-names; do
        [ -f "$d/$f" ] && printf '  %s = %s\n' "$f" "$(tr -d '\000' < "$d/$f" | head -c 60)"
    done
    ls "$d" | grep -i pinmux | sed 's/^/  pinmux-phandle: /'
done

echo
echo "=== 有没有 pinctrl 节点 ==="
ls -d /proc/device-tree/pinctrl* 2>/dev/null | head -5 || echo "  (没有 pinctrl 节点)"

echo
echo "=== gpio sysfs 是否可用 ==="
ls /sys/class/gpio 2>/dev/null | head -5 || echo "  (没有 /sys/class/gpio)"

echo
echo "=== 板端 eth/usb 与串口相关内核日志 ==="
dmesg 2>/dev/null | grep -iE 'serial|uart|ttyS' | tail -8
