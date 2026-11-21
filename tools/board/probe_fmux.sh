#!/bin/sh
# 只读探测：把 0x03001000 附近（未在任何驱动名下，疑似 TOP_MISC/FMUX）读出来看看
# 只是为了找「引脚复用寄存器」的位置，不做任何写操作
echo "=== /proc/iomem 里 0x03000000 段的占用情况（看哪些是空的）==="
grep -E '^0300' /proc/iomem 2>/dev/null

echo
echo "=== 读 0x03001000 起 0x100 字节（每行 4 个字）==="
A=0x03001000
i=0
while [ $i -lt 64 ]; do
    ADDR=$(printf '0x%08X' $((A + i * 4)))
    V=$(devmem $ADDR 32 2>/dev/null)
    printf '%s=%s  ' "$ADDR" "${V:-??}"
    i=$((i + 1))
    [ $((i % 4)) -eq 0 ] && echo
done

echo
echo "=== 读 0x03001100 起 0x100 字节 ==="
A=0x03001100
i=0
while [ $i -lt 64 ]; do
    ADDR=$(printf '0x%08X' $((A + i * 4)))
    V=$(devmem $ADDR 32 2>/dev/null)
    printf '%s=%s  ' "$ADDR" "${V:-??}"
    i=$((i + 1))
    [ $((i % 4)) -eq 0 ] && echo
done
