# Docker 交叉编译环境

把「Ubuntu 22.04 + cmake/make + 玄铁 riscv64 musl 工具链 + Sipeed SDK」打包成一个自包含镜像，
**换电脑时不用再配 WSL 环境**：拷一个 tar.gz 过去，`docker load` 就能编译。

## 内容物

| 项 | 来源 | 体积 |
|---|---|---|
| 基础系统 | `ubuntu:22.04` + cmake/make/gcc/g++/file/zlib1g/python3 | ~250 MB |
| 交叉工具链 | `env/toolchain/host-tools/gcc/riscv64-linux-musl-x86_64`（Xuantie-900 V2.6.1 / gcc 10.2.0） | 815 MB |
| Sipeed SDK | `env/sipeed-sdk-build`（include / lib / lib_dyn / kernel_include） | 58 MB |
| **镜像总计** | `licheerv-build:1.0` | **1.69 GB** |
| **导出 tar.gz** | `licheerv-build-image.tar.gz` | **322 MB** |

镜像里已经设好 `COMPILER=/opt/riscv64-musl/bin`、`CVI_SDK_ROOT=/opt/sipeed-sdk`，
所以容器内直接 `bash build.sh` 就行，不用再传环境变量。

## 三个脚本

| 脚本 | 做什么 |
|---|---|
| `build-image.sh` | 暂存工具链/SDK → `docker build` → `docker save` 导出 tar.gz |
| `compile.sh` | `docker run` 挂载本仓库 → 跑 `build.sh`，产物照旧落在 `build/bin/camera` |
| `verify-roundtrip.sh` | 删掉本地镜像 → 从 tar.gz 重新 `docker load` → 再编译一次（验证搬运件本身可用） |

两个都在 **WSL 里**用 `bash` 跑（不是 Windows PowerShell）。

## 在另一台电脑上怎么用

### 1. 装 Docker

- Windows：装 **Docker Desktop**，设置里勾选 *Use the WSL 2 based engine*；
- 或者任一 Linux 机器直接装 `docker`。

### 2. 导入镜像

```bash
docker load -i licheerv-build-image.tar.gz
docker images | grep licheerv-build     # 应看到 licheerv-build:1.0  1.69GB
```

### 3. 编译

**WSL / Linux：**

```bash
cd <仓库目录>
docker run --rm -v "$PWD":/work -w /work licheerv-build:1.0 bash ./build.sh
```

**Windows PowerShell（Docker Desktop）：**

```powershell
cd E:\study\LicheeRV\licheeRV-nano-greenTarget
docker run --rm -v "${PWD}:/work" -w /work licheerv-build:1.0 bash ./build.sh
```

产物在 `build/bin/camera`（`ELF 64-bit LSB executable, UCB RISC-V`）。

## 重建镜像（不想用导出的 tar.gz 时）

```bash
wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/docker/build-image.sh
```

可覆盖的环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `IMAGE` | `licheerv-build:1.0` | 镜像标签 |
| `SRC` | `/mnt/e/study/LicheeRV/env` | 工具链与 SDK 来源 |
| `OUT` | `/mnt/e/study/LicheeRV/licheerv-build-image.tar.gz` | 导出路径 |
| `EXPORT` | `1` | 设 0 只构建不导出 |
| `APT_MIRROR` | `mirrors.aliyun.com` | 置空则走官方 `archive.ubuntu.com` |
| `NETWORK` | `host` | `docker build --network`；容器内 DNS 不稳时用 host 更可靠 |

## 已验证 / 未验证

| 项 | 结果 |
|---|---|
| `gzip -t` 校验导出文件完整性 | ✅ 通过 |
| **删掉本地镜像后从 tar.gz 重新 `docker load`** | ✅ 通过，7 秒导入，镜像 `licheerv-build:1.0 1.69GB` |
| 导入后的镜像里工具链版本正确 | ✅ `Xuantie-900 ... Toolchain V2.6.1 / gcc 10.2.0` |
| 导入后的镜像能编译本工程 | ✅ `BUILD_OK` → `ELF 64-bit LSB executable, UCB RISC-V` |
| 工具链可重定位（复制到 `/opt` 仍能出 RISC-V 可执行文件） | ✅ 实测通过 |
| 镜像构建期自检（编译最小 C 程序并 `file` 验证） | ✅ 日志有 `TOOLCHAIN_SELFCHECK_OK` |
| 在**另一台电脑**上 load（不同 Docker 版本/OS） | ❌ **未验证**（需要第二台机器） |
| Docker Desktop（Windows 后端）下 bind mount 编译 | ❌ **未验证**（本机是 WSL 内的 docker 29.9.0） |

> 「删掉镜像再从 tar.gz 导入」这一步就是 `verify-roundtrip.sh`，
> 它模拟了换机场景，除"不同机器的 Docker 版本"外都已覆盖。
> 想看结果随时重跑：`wsl -e bash .../docker/verify-roundtrip.sh`

## 已知注意点

1. **工具链只要 `riscv64-linux-musl-x86_64` 这一个变体**。`env/toolchain` 里另外两个
   （`riscv64-linux-x86_64` 2.1G glibc 版、`riscv64-elf-x86_64` 653M 裸机版）用不到，
   全塞进去镜像会到 5G 以上。
2. **国内网络下构建必须换 apt 源**。`archive.ubuntu.com:80` 会不定时连不上，
   脚本默认已用阿里云镜像；换到别的网络环境时可能要改成别的源或置空。
3. **产物权限**：容器里以 root 跑，写到 Windows 挂载目录时属主可能显示异常，不影响使用。
4. 镜像不含板端部署脚本，部署仍然用仓库里的 `scp` 流程（见 `README.md` 的部署一节）。
