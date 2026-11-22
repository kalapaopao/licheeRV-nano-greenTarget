#!/bin/bash
# 端到端验证：删掉本地镜像 -> 从导出的 tar.gz 重新 load -> 再编译一次
set -euo pipefail
TGZ=/mnt/e/study/LicheeRV/licheerv-build-image.tar.gz
REPO=/mnt/e/study/LicheeRV/licheeRV-nano-greenTarget
IMAGE=licheerv-build:1.0

echo "=== 0) tar.gz 存在性 ==="
ls -lh "$TGZ" | awk '{print "  " $5, $NF}'
echo "  gzip 完整性检查:"
gzip -t "$TGZ" && echo "  OK（gzip 校验通过，文件没坏）"

echo
echo "=== 1) 删掉本地镜像（模拟一台干净的机器）==="
docker rmi -f "$IMAGE" >/dev/null 2>&1 && echo "  已删除 $IMAGE" || echo "  本地本来就没有"
docker images | grep -c licheerv-build || echo "  确认：镜像列表里已无 licheerv-build"

echo
echo "=== 2) 从 tar.gz 导入 ==="
time docker load -i "$TGZ" 2>&1 | sed 's/^/  /'

echo
echo "=== 3) 检查镜像 ==="
docker images "$IMAGE" --format '  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.ID}}'

echo
echo "=== 4) 容器内自检 + 编译 ==="
docker run --rm -v "$REPO":/work -w /work "$IMAGE" bash -c '
    echo "  COMPILER     = $COMPILER"
    echo "  CVI_SDK_ROOT = $CVI_SDK_ROOT"
    "$COMPILER/riscv64-unknown-linux-musl-gcc" --version | head -1 | sed "s/^/  /"
    bash ./build.sh 2>&1 | tail -5 | sed "s/^/  /"
' 2>&1

echo
echo "=== 5) 产物 ==="
ls -l "$REPO/build/bin/camera" | awk '{print "  " $5 " 字节  " $NF}'
file "$REPO/build/bin/camera" | sed 's/^/  /'
echo
echo "ROUNDTRIP_OK"
