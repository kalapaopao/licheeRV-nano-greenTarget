#!/bin/bash
# 用镜像编译本仓库（不污染宿主机，也不需要宿主机装任何交叉编译工具）
#
# 用法（WSL）：
#   wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/docker/compile.sh
#
# 产物照旧在 build/bin/camera（和直接在 WSL 里跑 build.sh 完全一样）。
#
# Windows 上的 Docker Desktop 用法（PowerShell，注意路径）：
#   docker run --rm -v E:\study\LicheeRV\licheeRV-nano-greenTarget:/work -w /work `
#     licheerv-build:1.0 bash build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
: "${IMAGE:=licheerv-build:1.0}"

echo "仓库: $REPO"
echo "镜像: $IMAGE"
echo

# 镜像里已经通过 ENV 设好 COMPILER / CVI_SDK_ROOT，这里直接跑 build.sh
docker run --rm \
    -v "$REPO":/work \
    -w /work \
    "$IMAGE" \
    bash ./build.sh

echo
echo "=== 宿主侧产物 ==="
ls -l "$REPO/build/bin/camera" 2>/dev/null || echo "（没找到产物，检查上面的输出）"
