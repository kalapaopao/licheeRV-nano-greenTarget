#!/bin/bash
# 展示端到端测试产出的真实文件（会话目录在 /tmp 下，测试跑完不删）
set -u
d=$(ls -1d /tmp/logpty_* 2>/dev/null | tail -1)
if [ -z "$d" ]; then echo "找不到测试产物目录"; exit 1; fi
echo "会话根目录: $d"
echo
echo "=== 文件清单 ==="
find "$d" -type f | sed "s|^$d/||"
echo
s=$(ls -1d "$d"/rec/sess_* 2>/dev/null | tail -1)
echo "=== meta.txt ==="
cat "$s/meta.txt"
echo
echo "=== mcu.csv（前 4 行）==="
head -4 "$s/mcu.csv"
echo
echo "=== frames.csv 表头（列数）==="
head -1 "$s/frames.csv"
awk -F, 'NR==1{print "表头列数 =", NF}' "$s/frames.csv"
echo
echo "=== frames.csv 数据行数 ==="
awk 'END{print NR-1}' "$s/frames.csv"
