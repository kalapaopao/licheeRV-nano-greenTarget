# 交叉编译环境镜像（LicheeRV Nano / SG2002）
#
# 目标：把「Ubuntu 22.04 + cmake/make + 玄铁 riscv64 musl 工具链 + Sipeed SDK 头文件库」
# 打成一个自包含镜像，换电脑时不用再配 WSL 环境，docker load 就能用。
#
# 构建请用 build-image.sh（它会先暂存工具链/SDK 再 build，避免整盘当上下文）。
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# apt 源。默认走官方；国内网络经常连不上 archive.ubuntu.com:80，
# 构建时可用 --build-arg APT_MIRROR=mirrors.aliyun.com 换成国内镜像。
ARG APT_MIRROR=""
RUN if [ -n "$APT_MIRROR" ]; then \
        sed -i "s|://archive.ubuntu.com|://${APT_MIRROR}|g; s|://security.ubuntu.com|://${APT_MIRROR}|g" \
            /etc/apt/sources.list; \
    fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        cmake \
        make \
        gcc \
        g++ \
        file \
        zlib1g \
        python3 \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# 玄铁工具链 Xuantie-900 V2.6.1（gcc 10.2.0，riscv64-unknown-linux-musl）。
# 只复制实际用到的 riscv64-linux-musl-x86_64 这一个变体：
#   riscv64-linux-x86_64（glibc 版，2.1G）和 riscv64-elf-x86_64（裸机版，653M）用不到。
# 实测该工具链可重定位（复制到 /opt 下仍能正常出 RISC-V 可执行文件）。
COPY toolchain /opt/riscv64-musl

# Sipeed 官方 SDK 产物目录：include/ lib/ lib_dyn/ kernel_include/
COPY sdk /opt/sipeed-sdk

ENV COMPILER=/opt/riscv64-musl/bin \
    CVI_SDK_ROOT=/opt/sipeed-sdk

# 自检：构建完就验证工具链能出 RISC-V 可执行文件，避免到用的时候才发现坏了
RUN printf 'int main(void){return 0;}\n' > /tmp/t.c \
 && "${COMPILER}/riscv64-unknown-linux-musl-gcc" -O2 /tmp/t.c -o /tmp/t \
 && file /tmp/t | grep -q 'UCB RISC-V' \
 && rm -f /tmp/t /tmp/t.c \
 && echo "TOOLCHAIN_SELFCHECK_OK"

WORKDIR /work
CMD ["/bin/bash"]
