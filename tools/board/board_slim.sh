#!/bin/sh
# 板端瘦身：只保留「运行必需」的东西。文档、旧数据、备份一律不留（都在 git 里）。
#
# 用法（在板子上跑，或 scp 到 /tmp 后 sh /tmp/board_slim.sh）：
#   sh tools/board/board_slim.sh          # 只清理
#   sh tools/board/board_slim.sh --dry    # 只报告会删什么
set -u
# 板端根目录：默认按脚本自身位置推（<root>/tools/board/x.sh → <root>），
# 也可以用 FLY_ROOT 覆盖
ROOT=${FLY_ROOT:-$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)}
[ -n "$ROOT" ] || ROOT=/code/fly_LicheeRV
cd "$ROOT" || exit 1
DRY=0
[ "${1:-}" = "--dry" ] && DRY=1

say() { echo "  $*"; }
del() {
    if [ -e "$1" ]; then
        if [ "$DRY" = "1" ]; then say "会删 $1"; else rm -rf "$1" && say "删了 $1"; fi
    fi
}

echo "=== 板端瘦身: $ROOT ==="
du -sh . 2>/dev/null | sed 's/^/  清理前 /'

# 1) 文档不在板端留（仓库里有）；tools/board 保留，运维脚本要能随时跑
del README.md
del docs

# 2) 旧录制/日志数据（数据是产物，不是代码）
del logs
del recordings
if [ -d record ]; then
    for d in record/sess_*; do [ -d "$d" ] && del "$d"; done
    for f in record/health_*.csv record/video_frames_*.csv record/*.avi; do
        [ -f "$f" ] && del "$f"
    done
fi

# 3) 手工备份一律不留（回退靠 git）
for f in *.bak *.bak_* *.new *.old; do
    [ -e "$f" ] && del "$f"
done
del backup

# 4) 运行状态历史：清空但保留文件（status.sh 会读）
if [ -f state/run_history.log ]; then
    if [ "$DRY" = "1" ]; then say "会清空 state/run_history.log"; else : > state/run_history.log; say "清空 state/run_history.log"; fi
fi

echo
echo "=== 现在的板端内容 ==="
ls -l
echo
du -sh . 2>/dev/null | sed 's/^/  清理后 /'
