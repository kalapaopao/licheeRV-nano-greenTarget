# WSL 交叉编译环境交接说明（`~/sipeed_sdk`）

> **写这份文档的原因**：WSL 里还留着一棵 3.7 GB 的 SDK 源码/编译树。
> 如果不记下来，将来有人（包括我自己）会忘记它的存在，或误以为要删却删掉关键输入。
> 本文说明：**它是什么、还需不需要、删了怎么重建**。

- 记录日期：2026-11-06
- 位置：WSL（Ubuntu 22.04，ext4 vhdx 在 `D:\WSL\Ubuntu-22.04\ext4.vhdx`）
- 路径：`/home/admin123/sipeed_sdk`（即 `~/sipeed_sdk`）
- 大小：**2.4 GB**

---

## 一、结论先行

| 问题 | 答案 |
|---|---|
| 日常编译本项目，需要它吗？ | **不需要**。编译只用 `E:\study\LicheeRV\env\toolchain` + `env\sipeed-sdk-build` |
| 它的编译产物在 Windows 有备份吗？ | **有，且字节一致**（见第三节实测比对） |
| 删掉会影响已跑通的 80fps 版本吗？ | **不影响**。产物二进制已存在且不依赖它 |
| 什么情况下才需要它？ | **只有**要重新交叉编译 middleware（VI/VPU/VENC/ISP 静态库）时 |
| 删了能重建吗？ | **能**，需要重新拉源码 + 编内核头，约 30~60 分钟（见第五节） |

**一句话**：它是「可再生的构建缓存」，不是唯一副本。Windows 侧的 `env/sipeed-sdk-build` 才是权威副本。

---

## 二、里面到底有什么

```
~/sipeed_sdk/                    2.4 GB   Sipeed 官方仓库稀疏克隆
├── middleware/                  259 MB   ★ 中间件源码（VI/VPU/VENC/ISP/SAMPLE）
│   └── v2/
│       ├── lib/                 60 MB    ★ 编译出的 26 个 .a
│       └── include/                     头文件
├── osdrv/                       226 MB   内核驱动 + CVITEK 私有 uapi 头
├── build/                        38 MB   ★ sophgo 构建脚本（cvisetup.sh / defconfig）
├── linux_5.10/                  1.2 GB  ★ 内核源码 + 已生成的内核头
│   └── build/sg2002_licheervnano_sd/riscv/usr/include/   ← 54 个头，编译必需
├── ramdisk/                     1.4 GB   （稀疏拉取时顺带拉的，本项目未用）
├── isp_tuning/                   19 MB   ISP 调参工具与参数
├── host/                        5.4 MB
├── u-boot-2021.10/              776 KB
├── install/                      20 KB   构建输出目录（空）
└── .git/                                HEAD = d4003f15b "fix reboot adbd issue"

sparse-checkout 启用路径：
  middleware, osdrv, build, linux_5.10, ramdisk,
  cvi_rtsp, tdl_sdk, demo, host, isp_tuning
```

---

## 三、与 Windows 归档的实测比对（2026-11-06）

Windows 侧权威副本：`E:\study\LicheeRV\env\sipeed-sdk-build\`

| 项目 | WSL `~/sipeed_sdk` | Windows `env/sipeed-sdk-build` | 结论 |
|---|---|---|---|
| 静态库数量 | 26 个（60 MB） | **28 个（50 MB）** | Windows 是超集 |
| `libvpu.a` | md5 `c15cd79f0e33` | md5 `c15cd79f0e33` | **字节一致** |
| `libvenc.a` | md5 `dd06ab2ab682` | md5 `dd06ab2ab682` | **字节一致** |
| `libisp.a` | md5 `1402df2292d6` | md5 `1402df2292d6` | **字节一致** |
| `libsample.a` | md5 `5b7d85cf4a7b` | md5 `5b7d85cf4a7b` | **字节一致** |
| `libsys.a` | md5 `e513e80a2069` | md5 `e513e80a2069` | **字节一致** |
| 内核头 `linux/` | 54 个 | **54 个** | 数量一致 |
| `cvi_type.h` | md5 `7debab5e0190` | md5 `7debab5e0190` | **字节一致** |
| `ion_cvitek.h` | md5 `d333549d20a8` | md5 `d333549d20a8` | **字节一致** |
| 编译必需的 5 个私有头 | 有 | 有（`cvi_type` / `cvi_common` / `cvi_defines` / `ion_cvitek` / `cvi_comm_video`） | 齐备 |

**结论：WSL 侧所有编译产物都已在 Windows 完整归档，且逐字节相同。**

---

## 四、最终产物到底依赖什么（容易记错的地方）

`licheeRV-nano-greenTarget/build/bin/camera`（451,816 字节）的运行时依赖：

```
libsys.so  libvpu.so  libvenc.so  libisp.so  libisp_algo.so
libae.so   libawb.so  libaf.so    libcvi_bin.so  libcvi_bin_isp.so
libatomic.so.1  libc.so
```

**重要**：这 12 个全部是**板端 `/mnt/system/usr/lib` 自带的 .so**，编译期从
`env/sipeed-sdk-build/lib_dyn/`（17 个 .so，从板子拷回来的）链接。

而 `env/sipeed-sdk-build/lib/` 下的那 28 个 `.a`，**最终链接只用到 `libini.a` 一个**
（inih，INI 解析器，`ini_parse`）。其余 27 个 `.a` 只是「重新编 middleware 的产物留档」，
不参与本项目链接。

> 为什么最终用板端 .so 而不是自编 .a：
> 自编 .a 与板端驱动的 ABI 在 720P 路径上有差异（曾表现为 `vi_sdk_enable_chn NG`
> 与全绿画面）。改用板端 .so + 源码编译 sensor/common 后，80fps 与颜色才同时正确。
> 详见 `docs/HANDOFF_ISSUE_720p_80fps.md`。

---

## 五、删掉后如何重建（完整步骤）

### 5.1 拉源码（WSL 内，不要用 Windows 端 clone，仓库含 Windows 非法文件名）

```bash
cd ~
git clone --filter=blob:none --sparse https://github.com/sipeed/LicheeRV-Nano-Build.git sipeed_sdk
cd sipeed_sdk
git sparse-checkout set middleware osdrv build linux_5.10 ramdisk cvi_rtsp tdl_sdk demo host isp_tuning
git submodule update --init --depth 1 middleware osdrv linux_5.10
```

### 5.2 生成内核头（middleware 编译必需）

```bash
TC=/mnt/e/study/LicheeRV/env/toolchain/host-tools/gcc/riscv64-linux-musl-x86_64/bin
export PATH="$TC:$PATH"
K=~/sipeed_sdk/linux_5.10
OUT=$K/build/sg2002_licheervnano_sd/riscv
mkdir -p "$OUT"
cd "$K"
make ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-musl- headers_install INSTALL_HDR_PATH="$OUT"
```

> `defconfig` 会 rc≠0，可忽略；`headers_install` 成功即可产出 `usr/include`。

### 5.3 补齐 CVITEK 私有头（关键，否则编译报 fatal error）

`headers_install` 只产出通用内核头，以下三类必须手工合并：

```bash
OUT=~/sipeed_sdk/linux_5.10/build/sg2002_licheervnano_sd/riscv/usr/include/linux
O=~/sipeed_sdk/osdrv/interdrv/v2/include
K=~/sipeed_sdk/linux_5.10

mkdir -p "$OUT"
# ① 通用 uapi（含 cvi_type.h / cvi_common.h 等）
cp -n $O/common/uapi/linux/*.h        "$OUT"/
# ② 芯片相关（cvi_defines.h 在 mars/phobos 两套里，本项目用 mars）
cp -n $O/chip/mars/uapi/linux/*.h     "$OUT"/
# ③ 内核 staging 目录（ion_cvitek.h，headers_install 不含 staging）
cp -n $K/drivers/staging/android/uapi/*.h "$OUT"/
```

最终应有 54 个 `.h`。

### 5.4 生成配置并编译 middleware

```bash
TC=/mnt/e/study/LicheeRV/env/toolchain/host-tools/gcc/riscv64-linux-musl-x86_64/bin
export PATH="$TC:$PATH"
export CROSS_COMPILE_MUSL_RISCV64="$TC/riscv64-unknown-linux-musl-"
cd ~/sipeed_sdk/build
source ./cvisetup.sh
defconfig sg2002_licheervnano_sd      # 生成 .config，PROJECT_FULLNAME=sg2002_licheervnano_sd
build_middleware
```

已知会失败的两处，需分别补：

**① `-lc` 找不到**（链接 `.so` 时）—— 把 sysroot 库软链到 gcc 默认搜索目录：

```bash
TC=/mnt/e/study/LicheeRV/env/toolchain/host-tools/gcc/riscv64-linux-musl-x86_64
SRC=$TC/sysroot/usr/lib64/lp64d
DST=$TC/riscv64-unknown-linux-musl/lib
for f in "$SRC"/*; do b=$(basename "$f"); [ -e "$DST/$b" ] || ln -s "$f" "$DST/$b"; done
```

**② `libcvi_json-c source tarball not exist`**（modules/bin 需要）—— 该目标跳过即可，
本项目的 `libcvi_bin` 用板端 `libcvi_bin.so` 代替（已归档在 `lib_dyn/`）。

### 5.5 逐个模块编译（`build_middleware` 覆盖不到的）

`build_middleware` 只到 `modules/sys`，以下模块需单独编，**必须显式传三个参数**，
否则报 `PROJECT_FULLNAME not defined`：

```bash
cd ~/sipeed_sdk/build && source ./cvisetup.sh && defconfig sg2002_licheervnano_sd
export CROSS_COMPILE_MUSL_RISCV64="$TC/riscv64-unknown-linux-musl-"

cd ~/sipeed_sdk/middleware/v2/modules/vpu      # ★ VI/VPSS 在这里，不在 modules/vi
make SDK_VER=musl_riscv64 CHIP_ARCH=SG200X PROJECT_FULLNAME=sg2002_licheervnano_sd
cd ../venc  && make SDK_VER=musl_riscv64 CHIP_ARCH=SG200X PROJECT_FULLNAME=sg2002_licheervnano_sd
cd ../isp   && make SDK_VER=musl_riscv64 CHIP_ARCH=SG200X PROJECT_FULLNAME=sg2002_licheervnano_sd
cd ../misc  && make SDK_VER=musl_riscv64 CHIP_ARCH=SG200X PROJECT_FULLNAME=sg2002_licheervnano_sd
cd ~/sipeed_sdk/middleware/v2/sample && make SDK_VER=musl_riscv64 CHIP_ARCH=SG200X PROJECT_FULLNAME=sg2002_licheervnano_sd
```

产出在 `~/sipeed_sdk/middleware/v2/lib/*.a`。

### 5.6 归档回 Windows

```bash
M=~/sipeed_sdk/middleware/v2
D=/mnt/e/study/LicheeRV/env/sipeed-sdk-build
cp -a $M/lib/*.a         "$D/lib"/
cp -a $M/lib/3rd/*.a     "$D/lib"/
cp -a $M/include/*       "$D/include"/
cp -a $M/sample/common/*.h "$D/include/sample"/
# 补充 modules/*/include 下的头（扁平化）
for f in $(find $M/modules -path '*include*' -name '*.h'); do cp -n "$f" "$D/include"/; done
cp -a ~/sipeed_sdk/linux_5.10/build/sg2002_licheervnano_sd/riscv/usr/include "$D/kernel_include"
```

---

## 六、删除方法（确认不需要重建 middleware 时）

```bash
# WSL 内
rm -rf ~/sipeed_sdk
# 释放后可在 Windows 侧压缩 vhdx（可选，回收宿主磁盘）
# wsl --shutdown
# Optimize-VHD -Path D:\WSL\Ubuntu-22.04\ext4.vhdx -Mode Full   （需 Hyper-V 模块）
```

**删除前自检清单**：

- [ ] `E:\study\LicheeRV\env\sipeed-sdk-build\lib\` 有 28 个 `.a`
- [ ] `env\sipeed-sdk-build\lib_dyn\` 有 17 个 `.so`
- [ ] `env\sipeed-sdk-build\kernel_include\linux\` 有 54 个 `.h`
- [ ] `env\sipeed-sdk-build\include\` 递归共 87 个 `.h`（顶层 70 + `isp/` 13 + `sample/` 4）
- [ ] `wsl -e bash env/verify_build.sh` 能编出 451816 字节的产物

全部打勾即可安全删除。

---

## 七、遗留提醒

1. **WSL 里另有约 43 GB 不属于本项目**（`~/projects` 26 GB、`~/.local` 7.1 GB、
   `~/miniconda3` 3.4 GB、`~/venvs` 2.7 GB 等）。这些与 LicheeRV 无关，**不要动**。
2. **`~/sipeed_sdk/ramdisk` 占 1.4 GB**，本项目从未使用，只是稀疏拉取时顺带下来的。
   若只想保留重建能力，可以 `rm -rf ~/sipeed_sdk/ramdisk` 先省 1.4 GB。
3. Windows 侧 `env/` 整个目录被 `.gitignore` 忽略（4 GB），**不会进 git**，所以不会随
   仓库分发。克隆仓库后必须按本文档 5.1~5.6 重建，或用板端 `lib_dyn` 的 .so 直接编译。

---

## 八、相关文档

| 文档 | 内容 |
|---|---|
| `docs/HANDOFF_ISSUE_720p_80fps.md` | 720P/80fps 卡点的完整取证与提问清单 |
| `docs/architecture/` | 5 页架构图（总体/逐帧/算法/启动/诊断树） |
| `README.md` | 项目功能、编译、运行、已知限制 |
