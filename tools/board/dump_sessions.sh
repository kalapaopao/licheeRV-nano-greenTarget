#!/bin/sh
# 导出这次飞行的数据摘要
cd /code/fly_LicheeRV || exit 1

echo "=== record/ 目录 ==="
ls -1t record/ 2>/dev/null | head -10
echo
echo "=== 会话目录 ==="
for S in $(ls -1dt record/sess_* 2>/dev/null | head -3); do
    echo "--- $S ---"
    ls -l "$S" | awk 'NR>1{printf "   %10s  %s\n", $5, $NF}'
    echo "   meta.txt:"
    sed 's/^/     /' "$S/meta.txt"
    echo
done
