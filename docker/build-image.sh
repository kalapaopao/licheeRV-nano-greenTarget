#!/bin/bash
# 构建交叉编译镜像，并导出成可以拷到别的电脑的 tar.gz
#
# 用法（在 WSL 里跑，不是 Windows）：
#   wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/docker/build-image.sh
#
# 可直接覆盖的环境变量：
#   IMAGE       镜像标签               默认 licheerv-build:1.0
#   SRC         工具链/SDK 来源根目录   默认 /mnt/e/study/LicheeRV/env
#   OUT         导出的 tar.gz 路径      默认 $SRC/../licheerv-build-image.tar.gz
#   EXPORT      0 = 只构建不导出        默认 1
#   APT_MIRROR  apt 源域名              默认 mirrors.aliyun.com（国内网络更稳；
#                                       置空则走官方 archive.ubuntu.com）
#   NETWORK     build 时的网络模式      默认 host（容器内 DNS 不稳时更可靠）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${IMAGE:=licheerv-build:1.0}"
: "${SRC:=/mnt/e/study/LicheeRV/env}"
: "${OUT:=$(cd "$SRC/.." && pwd)/licheerv-build-image.tar.gz}"
: "${EXPORT:=1}"
: "${APT_MIRROR:=mirrors.aliyun.com}"
: "${NETWORK:=host}"
: "${CTX:=/tmp/licheerv-docker-ctx}"

TC_SRC="$SRC/toolchain/host-tools/gcc/riscv64-linux-musl-x86_64"
SDK_SRC="$SRC/sipeed-sdk-build"

echo "=== 检查来源 ==="
[ -x "$TC_SRC/bin/riscv64-unknown-linux-musl-gcc" ] || { echo "缺少工具链: $TC_SRC"; exit 1; }
[ -d "$SDK_SRC/include" ] || { echo "缺少 SDK: $SDK_SRC"; exit 1; }
echo "  toolchain   = $TC_SRC"
echo "  sdk         = $SDK_SRC"
echo "  apt mirror  = ${APT_MIRROR:-<官方>}"
echo "  build net   = $NETWORK"

echo
echo "=== 构建上下文（只放真正需要的）==="
rm -rf "$CTX"
mkdir -p "$CTX"
cp -a "$TC_SRC" "$CTX/toolchain"
cp -a "$SDK_SRC" "$CTX/sdk"
cp "$HERE/Dockerfile" "$CTX/Dockerfile"
du -sh "$CTX"/toolchain "$CTX"/sdk

echo
echo "=== docker build ==="
cd "$CTX"
docker build --network="$NETWORK" --build-arg "APT_MIRROR=$APT_MIRROR" -t "$IMAGE" .

echo
echo "=== 镜像信息 ==="
docker images "$IMAGE" --format '  {{.Repository}}:{{.Tag}}  {{.Size}}'

if [ "$EXPORT" = "1" ]; then
    echo
    echo "=== 导出到 $OUT ==="
    docker save "$IMAGE" | gzip -1 > "$OUT"
    ls -lh "$OUT" | awk '{print "  大小: " $5}'
    echo "  换电脑后：docker load -i \"$OUT\""
fi

echo
echo "BUILD_IMAGE_OK"
