# licheeRV-nano-greenTarget

LicheeRV Nano（SG2002 + GC4653）80fps 取流工程 + 绿色圆灯传统视觉检测。

在原 `fly_LicheeRV` 取流项目基础上集成 PC 端验证过的 `green_target` 检测算法（C++ 移植版），
并在板端可交叉编译复现。

## 功能

- 1280×720 取流 + 硬件 JPEG 编码 + MJPEG 网页预览（原 80fps 工程）
- **绿色发光圆灯检测**：YUV 域判据 + 连通域 + 形状过滤 + 最近邻跟踪（每帧 ~2.25ms @1GHz）
- YAML 配置（`config.yaml`）：曝光/增益/JPEG 质量/运行时长/**录像开关**
- `record_enable: true` 时每次启动自动录像到 `recordings/*.avi`，无需命令行参数

## 目录

```
src/camera.cpp          主程序：VI 取流 → VENC JPEG → HTTP 预览 + 可选 AVI 录像 + 检测探针
src/green_detect.{h,cpp} 检测算法（纯 C++11，无第三方依赖）
src/avi_mjpeg.h         AVI/MJPEG 封装
tools/detect_cli.cpp    独立检测命令行（对单帧 I420 跑检测，用于离线验证）
tools/test_detect.cpp   主机端自测入口
tests/                  回归测试帧与期望结果（expect.json）
CMakeLists.txt          交叉编译配置
build.sh                一键编译（WSL 执行）
start.sh / stop.sh      板端启停脚本（YAML 解析、36MHz 时钟切换、相机锁、温度保护）
config.yaml             板端配置（含 record_enable 开关）
```

## 交叉编译（在 WSL 中，文件全在 Windows 侧）

依赖：
- 交叉工具链：`E:\study\LicheeRV\env\toolchain`（Xuantie-900 musl gcc 10.2.0）
- SDK 静态库：`E:\study\LicheeRV\env\sipeed-sdk-build`（自 Sipeed 官方源码编译的 28 个 .a + 头文件）

```powershell
wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/build.sh
# 产物：build/bin/camera（动态依赖仅 libcvi_bin.so / libatomic.so.1 / libc，与原版一致）
```

## 主机端自测（不依赖板子）

```bash
cd licheeRV-nano-greenTarget
g++ -O2 -std=c++11 -o /tmp/test_detect tools/test_detect.cpp src/green_detect.cpp -lm
/tmp/test_detect tests/gc4653_20261031_093856_740_3600.i420 1280 720 4
# 期望：DETECTED grid_c=(155.4,62.8) full_c=(621.4,251.1)
```

## 板端运行

```sh
cd /code/fly_LicheeRV
sh start.sh --check-config     # 校验配置
sh start.sh                    # 预览（时长按 config.yaml 的 run_seconds）
sh start.sh 60                 # 指定 60 秒
sh stop.sh                     # 停止（另一终端或 Ctrl+C）
```

- 预览地址：`http://<板子IP>:8081/`（USB 网卡一般 `10.133.124.1`）
- 录像：`config.yaml` 里 `record_enable: true` 后每次启动自动录到 `recordings/`
- 检测输出：日志中 `GREEN_TARGET status=detected cx=.. cy=.. r=..`

## 已知限制

- **帧率 41.5fps**（原工程为 80fps）：开源 Sipeed SDK 的 GC4653 驱动只有 1440P@30 寄存器组，
  已打补丁让它接受 720P 尺寸，但 80fps 需要额外的 720P@60/80 寄存器表（Sipeed 未开源）。
- 停止请用 `sh stop.sh` 或 Ctrl+C，**不要 kill -9**（会残留 VPSS 状态，需重启板子）。
- 录像码率约 4MB/s，长录注意 TF 卡空间（`recordings/` 需定期清理）。

## 代码怎么读

不知道从哪里开始看 → **[docs/READING_GUIDE.md](docs/READING_GUIDE.md)**

一句话版：真正要读的只有约 1100 行（src/camera.cpp 493 + src/green_detect.cpp 335
+ green_detect.h 113 + vi_mjpeg.h 160）；src/common/ 和 src/sensor/ 下约 7000 行
是厂商 sample 代码，只在报错时查。

推荐顺序：

1. green_detect.h 22~50 行 —— 全部可调参数
2. green_detect.cpp 122~267 行 —— 检测主流程
3. green_detect.cpp 272~338 行 —— 跟踪三态机
4. camera.cpp 319~497 行 —— 主循环（核心）
5. camera.cpp 158~191 行 —— 编码线程

---

## 环境与重建

编译只需要 Windows 侧的 `E:\study\LicheeRV\env\`，**WSL 只当执行器**（文件全在 Windows）：

| 目录 | 内容 | 编译时是否必需 |
|---|---|---|
| `toolchain/` | RISC-V musl 交叉工具链（Xuantie-900 gcc 10.2.0） | 必需 |
| `sipeed-sdk-build/include/` | 87 个头（顶层 70 + `isp/` 13 + `sample/` 4） | 必需 |
| `sipeed-sdk-build/kernel_include/` | 54 个内核 + CVITEK 私有 uapi 头 | 必需 |
| `sipeed-sdk-build/lib_dyn/` | **17 个板端 .so**（从板子拷回，链接期使用） | 必需 |
| `sipeed-sdk-build/lib/` | 28 个自编 `.a`；最终链接**只用 `libini.a`** | 部分 |

> **WSL 里另有一棵 3.7 GB 的 `~/sipeed_sdk` 源码/编译树**，仅用于「重新交叉编译
> middleware」，日常编译本项目**不需要**它。它是什么、能否删、怎么重建，见
> [docs/HANDOFF_WSL_BUILD_ENV.md](docs/HANDOFF_WSL_BUILD_ENV.md)。

### 编译方式与本项目的构成

最终产物 = **板端 .so（动态链接）+ 本项目源码编译的驱动与 sample 层**：

```
src/sensor/      用户态 GC4653 驱动（含真正的 720P 寄存器表）
src/common/      项目自带的 sample 层（720P 模式按 1280x720 配置）
src/camera.cpp   主程序
src/green_detect.cpp  检测算法
```

链接：`lib_dyn` 里的板端 `.so`（`libsys/libvpu/libvenc/libisp/...`）+ 静态 `libini.a`。

### 为什么不用自编的 .a 去链接

自编 `.a` 与板端驱动在 720P 路径上 ABI 不一致，曾表现为
`vi_sdk_enable_chn NG` 与**全绿画面**。改用板端 `.so` + 源码编译
`sensor/common` 之后，80fps 与正确颜色才同时成立。
完整取证见 [docs/HANDOFF_ISSUE_720p_80fps.md](docs/HANDOFF_ISSUE_720p_80fps.md)。

### 已验证的产物

```
build/bin/camera   451,816 字节   RISC-V musl
运行时依赖 12 个板端 .so，均存在于 /mnt/system/usr/lib
```

