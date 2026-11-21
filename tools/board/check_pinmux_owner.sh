#!/bin/sh
# 查这块板子的引脚复用（pinmux）到底是由谁配置的、Linux 侧有没有驱动
echo "=== ① dmesg 里有没有 pinmux/pinctrl 相关 ==="
dmesg 2>/dev/null | grep -iE 'pinmux|pinctrl|pad' | head -10
echo "(以上为空 = 内核没有配置过引脚复用)"

echo
echo "=== ② 内核符号里有没有 pinmux 驱动函数 ==="
grep -icE 'pinmux|cvitek_pin|sophgo_pin' /proc/kallsyms 2>/dev/null
grep -iE 'pinmux_(set|get)_mux|pinctrl_register' /proc/kallsyms 2>/dev/null | head -5

echo
echo "=== ③ 有没有 pinctrl 模块 ==="
find /lib/modules -iname '*pinctrl*' -o -iname '*pinmux*' 2>/dev/null | head -10
echo "(空 = 没有独立模块，可能是内置或完全没有)"

echo
echo "=== ④ 设备树里所有 pinctrl 相关节点 ==="
find /proc/device-tree -maxdepth 2 -iname '*pinctrl*' -o -maxdepth 2 -iname '*pinmux*' 2>/dev/null | head -10
echo "(空 = 设备树里没有 pinctrl 节点)"

echo
echo "=== ⑤ /sys/kernel/debug/pinctrl 内容 ==="
ls -la /sys/kernel/debug/pinctrl 2>/dev/null

echo
echo "=== ⑥ 各 UART 的 compatible 与 status（看有没有 pinctrl 引用）==="
for d in /proc/device-tree/serial@*; do
    [ -d "$d" ] || continue
    n=$(basename "$d")
    st=$(tr -d '\000' < "$d/status" 2>/dev/null)
    echo "  $n status=$st 属性: $(ls "$d" 2>/dev/null | tr '\n' ' ')"
done

echo
echo "=== ⑦ 有没有厂商的 pinmux 工具 / proc 接口 ==="
ls /proc/cvitek /proc/sophgo /sys/class/pinctrl 2>/dev/null
which pinmux 2>/dev/null
echo "(空 = 没有)"
