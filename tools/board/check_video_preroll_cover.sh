#!/bin/sh
# 核实：视频（video_frames.csv）到底有没有覆盖「触发前 3 秒」
# 对比 frames.csv 首帧 与 video_frames.csv 首帧 相对触发时刻的位置
cd /code/fly_LicheeRV || exit 1
S=$(ls -1dt record/sess_* 2>/dev/null | head -1)
[ -n "$S" ] || { echo "没有会话"; exit 0; }
echo "会话: $S"
echo
TRIG=$(grep '^trigger_mono_us' "$S/meta.txt" | cut -d= -f2)
echo "trigger_mono_us = $TRIG"
echo "cfg: $(grep '^cfg preroll' "$S/meta.txt")"
echo
CF=$(awk -F, 'NR==2{print $2}' "$S/frames.csv")
CFL=$(tail -1 "$S/frames.csv" | awk -F, '{print $2}')
echo "frames.csv      首帧 = $CF"
echo "frames.csv      末帧 = $CFL"
awk -v t="$TRIG" -v f="$CF" 'BEGIN{printf "  -> 首帧相对触发 = %+.3f s\n", (f-t)/1e6}'
echo
VF=$(awk -F, 'NR==2{print $3}' "$S/video_frames.csv")
VFL=$(tail -1 "$S/video_frames.csv" | awk -F, '{print $3}')
echo "video_frames.csv 首帧 = $VF"
echo "video_frames.csv 末帧 = $VFL"
awk -v t="$TRIG" -v f="$VF" 'BEGIN{printf "  -> 首帧相对触发 = %+.3f s\n", (f-t)/1e6}'
echo
echo "=== 结论 ==="
awk -v t="$TRIG" -v c="$CF" -v v="$VF" 'BEGIN{
  cd=(c-t)/1e6; vd=(v-t)/1e6;
  printf "  数据(CSV) 从触发前 %.2f 秒开始\n", -cd;
  printf "  视频      从触发前 %.2f 秒开始", -vd;
  if (vd > -1.0) printf("  <= 没有起飞前画面\n"); else printf("  <= 有起飞前画面\n");
}'
echo
echo "=== 视频帧数 vs 数据帧数 ==="
echo "  frames.csv       $(($(wc -l < "$S/frames.csv") - 1)) 行"
echo "  video_frames.csv $(($(wc -l < "$S/video_frames.csv") - 1)) 行"
