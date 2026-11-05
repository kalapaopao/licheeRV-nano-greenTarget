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

## 环境与重建

`E:\study\LicheeRV\env\` 下保存了完整交叉编译环境：

| 目录 | 内容 |
|---|---|
| `toolchain/` | RISC-V musl 交叉工具链 |
| `sipeed-sdk-build/` | 自编 SDK 静态库（libvpu/libvenc/libisp/libsample/libsns_gc4653 等 28 个）+ 头文件 + 内核 uapi 头 |
| `sipeed-sdk/` | Sipeed 官方 SDK 源码（middleware/osdrv/build，可重建上表静态库） |

SDK 驱动补丁（相对官方源码）：
1. `middleware/v2/component/isp/sensor/sg200x/gcore_gc4653/gc4653_cmos.c` —
   `cmos_set_image_mode` 接受 1440P 范围内的任意尺寸（原版只认 1440P@≤30fps）
2. 链接时**排除 `libsns_full.a`**（内含旧版 GC4653 驱动，会覆盖补丁版）
