#!/bin/bash
# 交叉编译 fly_LicheeRV（GC4653 80FPS 取流 + 绿色目标检测）
#
# 全部输入输出都在仓库目录内，WSL 只借用工具链与 SDK，不保存任何文件。
# 用法（Windows）：
#   wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/build.sh
#
# 可用环境变量覆盖：
#   COMPILER      交叉工具链 bin 目录
#   CVI_SDK_ROOT  平台 SDK 根目录
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$REPO/build"

# 默认工具链与 SDK 位置（与本机既有环境一致）
: "${COMPILER:=/mnt/e/study/LicheeRV/dartvision_build/wsl_env/toolchain/host-tools/gcc/riscv64-linux-musl-x86_64/bin}"
: "${CVI_SDK_ROOT:=/mnt/e/study/LicheeRV/dartvision_build/src_work/sdk}"
export COMPILER CVI_SDK_ROOT

echo "=== 环境 ==="
echo "COMPILER     = $COMPILER"
echo "CVI_SDK_ROOT = $CVI_SDK_ROOT"
[ -x "$COMPILER/riscv64-unknown-linux-musl-gcc" ] || { echo "缺少交叉工具链"; exit 1; }
[ -d "$CVI_SDK_ROOT/middleware/include" ] || { echo "缺少平台 SDK"; exit 1; }
"$COMPILER/riscv64-unknown-linux-musl-gcc" --version | head -1

echo "=== 配置 ==="
rm -rf "$BUILD"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > /tmp/fly_cmake.log 2>&1 || {
  echo "CMake 配置失败："; tail -20 /tmp/fly_cmake.log; exit 1; }

echo "=== 编译 ==="
cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -6

echo "=== 产物 ==="
ls -l "$BUILD/bin/camera"
file "$BUILD/bin/camera"
echo BUILD_OK