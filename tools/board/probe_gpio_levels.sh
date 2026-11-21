#!/bin/sh
# 只读实验：SG2002 的 GPIOA 块在 0x03020000。
# 思路：GPIO 的"输入数据"寄存器通常能反映引脚真实电平（不管它被复用成什么功能）。
#   A) 先让内核往 console(UART0_TX) 狂发字节，采样 GPIOA → 哪个 bit 在翻，哪个就是 UART0_TX 那根
#      这一步同时验证"采样法有效"
#   B) 停止 console 发送，采样 GPIOA → 如果此时还有 bit 在翻，那就是电控 TX 落在的引脚
echo "=== GPIOA 块前 8 个字（找数据寄存器）==="
i=0
while [ $i -lt 8 ]; do
    ADDR=$(printf '0x%08X' $((0x03020000 + i * 4)))
    printf '%s=%s  ' "$ADDR" "$(devmem $ADDR 32 2>/dev/null)"
    i=$((i + 1))
done
echo

sample_pins() {
    LABEL="$1"
    SECS="$2"
    # 采 400 次，记录出现过 0 和 1 的 bit
    OLD=""
    CHANGED=0
    n=0
    while [ $n -lt 400 ]; do
        V=$(devmem 0x03020000 32 2>/dev/null)
        [ -z "$V" ] && break
        if [ -n "$OLD" ] && [ "$V" != "$OLD" ]; then
            CHANGED=$((CHANGED + 1))
            # 记录变化的 bit
            D=$((V ^ OLD))
            b=0
            while [ $b -lt 32 ]; do
                if [ $(( (D >> b) & 1 )) -eq 1 ]; then
                    echo "    A$b 变过（GPIOA bit $b）"
                fi
                b=$((b + 1))
            done
            OLD=$V
        elif [ -z "$OLD" ]; then
            OLD=$V
        fi
        n=$((n + 1))
    done
    echo "  $LABEL：采样 $n 次，发现 $CHANGED 次电平变化"
}

echo
echo "=== A) console 狂发 → 看哪个 bit 翻（预期是 UART0_TX）==="
(
  E=0
  while [ $E -lt 60 ]; do
      printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789' > /dev/ttyS0 2>/dev/null
      E=$((E + 1))
  done
) &
SPAM=$!
sample_pins "console 发送中" 2
kill $SPAM 2>/dev/null
wait $SPAM 2>/dev/null

echo
echo "=== B) 停发 1 秒后再采样 → 若还有 bit 翻，就是电控的引脚 ==="
sleep 1
sample_pins "静默期" 2

echo
echo "=== 采样结束（全程只读，没有写过任何寄存器）==="
