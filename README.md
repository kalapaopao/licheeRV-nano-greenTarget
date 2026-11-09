# licheeRV-nano-greenTarget

LicheeRV Nano（SG2002 + GC4653）80fps 取流工程 + 绿色圆灯传统视觉检测。

在原 `fly_LicheeRV` 取流项目基础上集成 PC 端验证过的 `green_target` 检测算法（C++ 移植版），
并在板端可交叉编译复现。

## 功能

- 1280×720 取流 + 硬件 JPEG 编码 + MJPEG 网页预览（**实测 79.95fps**）
- **绿色发光圆灯检测**：YUV 域判据 + 连通域 + 形状过滤（每帧 4.96ms 采样 + 0.47ms 跟踪）
- **多目标筛选 + 卡尔曼跟踪**：加权评分选一个目标，角度域 2 状态滤波
- **角度输出**：像素→角度（针孔模型），供云台/电控闭环
- **串口输出**：50Hz ASCII 帧，含角度/角速度/距离/数据年龄/状态
- **时间戳与延迟可观测**：取帧时刻/帧号/原始 PTS 随结果传递；串口发送时重算端到端年龄（只进日志）；每 5 秒给出采集/检测/发送各段耗时
- YAML 配置（24 个参数）：曝光/增益/JPEG 质量/运行时长/录像/显示/检测/标定/串口
- `record_enable: true` 时每次启动自动录像到 `recordings/*.avi`，无需命令行参数
- 检测框画进 JPEG：网页预览、单帧抓图、录像三处都能看到

## 目录

```
src/camera.cpp          主程序：VI 取流 → 检测 → 画框 → VENC JPEG → HTTP/AVI
src/green_detect.{h,cpp}  检测算法（纯 C++11，无第三方依赖）
src/target_track.{h,cpp}  多目标筛选 + 卡尔曼（角度域）+ 像素→角度换算
src/uart_out.{h,cpp}      串口输出线程（ASCII + XOR 校验）
src/frame_timing.h      帧时间戳（帧号/采集时刻/PTS/dt/板端年龄，header-only）
src/avi_mjpeg.h         AVI/MJPEG 封装
tools/detect_cli.cpp    独立检测命令行（对单帧 I420 跑检测，用于离线验证）
tools/test_detect.cpp   主机端自测入口
tests/                  回归测试帧与期望结果 + 阶段1 算法单元测试
CMakeLists.txt          交叉编译配置
build.sh                一键编译（WSL 执行）
start.sh / stop.sh      板端启停脚本（YAML 解析、36MHz 时钟切换、相机锁、温度保护）
config.yaml             板端配置（手动运行）
config_boot.yaml        板端配置（开机自启，run_seconds=0）
docs/                   架构图、代码导读、标定指南、串口协议、交接文档
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
- 检测输出：日志中 `GREEN_TARGET state=confirmed yaw=.. pitch=.. dist=.. age=.. frame=..`
  （限频 10Hz）；每 5 秒另有 `DETECT` / `TIMING` / `PTS_STAT` / `UART` 统计行，
  字段含义见 [docs/UART_PROTOCOL.md](docs/UART_PROTOCOL.md) 第六节
- 串口输出：`uart_enable: true` 后把角度发给电控，协议见
  **[docs/UART_PROTOCOL.md](docs/UART_PROTOCOL.md)**

## 阶段1：多目标筛选 + 卡尔曼 + 角度 + 串口

面向电控联调的一层，**不改变取流架构**（仍在主线程内联，仍 80fps）。

### 做了什么

| 环节 | 实现 |
|---|---|
| 多候选收集 | `Detector::detectMulti()` 收集全部通过形状过滤的连通域（不再只返回最优） |
| 多目标筛选 | 评分 = `sel_conf_weight × 质量分 + (1-该值) × 离预测位置远近`，选最高分 |
| 卡尔曼 | 角度域 2 状态（角度 + 角速度），yaw/pitch 各一个，匀速模型 |
| 像素→角度 | 针孔模型 `atan((px-cx)/fx)`，含光轴补偿与符号翻转开关 |
| 状态机 | confirmed / coasting / lost 三态，`sel_confirm_frames` + `sel_lost_frames` 双门限 |
| 单目测距 | `距离 = f × 目标直径 / 像素直径`，含横向斜距修正 |
| 串口输出 | 独立线程，固定 50Hz，ASCII + XOR 校验，**无目标也发心跳** |

### 瞄准点偏移（打装甲板，不是打绿灯中心）

比赛里真正要打的是**绿灯上方装甲板的中心**。做法抄自 HERO `dart_vision_26`：
**瞄准点 = 灯中心上方 `factor × 灯直径`**，`factor` 由 `config.yaml` 的
`target_up_offset_factor` 控制（默认 1.5，与 HERO 的 `TARGET_UP_OFFSET_HEIGHT_FACTOR` 一致）。

- 因为偏移量按"检测框高度"缩放，所以**与距离无关**，远近都自动对；
- 物理含义：`factor = 装甲板中心到灯中心的距离 / 灯直径`（HERO：1.5 × 0.055m ≈ 82.5mm）；
- `factor = 0` 就是原来的"打灯中心"；联调时按实际弹着点微调（0.0~5.0）；
- 串口/日志里的 `yaw/pitch` 是**瞄准点**角度；日志另打 `lyaw/lpitch` 是**灯中心**角度，用于对照；
- 测距仍用灯的像素直径（`target_diameter_m`），与偏移无关。

### 时间戳与延迟观测（阶段1.1）

取帧处建立帧元数据（`src/frame_timing.h`），随结果一路传到串口线程：

| 项 | 实现 |
|---|---|
| 帧号 / 采集时刻 / PTS | `CVI_VI_GetChnFrame()` 成功后立刻记录；`u64PTS` 只记录不换算（板端实测为微秒，80fps 时每帧差≈12500） |
| 跟踪 dt | 用**采集时刻差**（不再用检测后的 `now()` 差），抽样/检测/日志/调度抖动不再混进卡尔曼；`detect_stride>1` 时 dt 自然等于跨帧采集间隔 |
| 串口年龄 | 发送线程按快照重算「取帧→**开始**写串口」年龄，**只写日志**（`uart_write_start_age_*`），ASCII 字段数量与顺序不变 |
| 日志限频 | `GREEN_TARGET` ≤10Hz（原来连续目标时 80Hz）；`DETECT`/`TIMING`/`PTS_STAT`/`UART` 每 5 秒 |
| 新增统计 | `proc_ms`（抽样→结果）、`lat_ms`（取帧→结果）、`last_frame_id`、`last_capture_age_ms`、原始 PTS 差值 |

⚠️ 串口协议里 `age` 的语义**没有变**：它仍是「距最近一次真实检测的毫秒数」，用于电控判断
数据陈旧度；「取帧→发送」的板端年龄只进日志。原因与阈值说明见
[docs/UART_PROTOCOL.md](docs/UART_PROTOCOL.md) 第二节、第三节与第六节。

板端实测（2026-11-08，80fps，`display_enable=false`）：`proc_avg≈5.1ms`、`lat_avg≈5.1ms`、
`uart_write_start_age_avg≈11.5ms`（50Hz 发送 vs 80Hz 出结果）；`PTS_STAT` 每帧差值≈12500 →
**`u64PTS` 单位是微秒**（代码仍只记录、不参与计算）。

### 板端实测（1280×720，无目标场景）

```
host_fps   = 79.95          编码 79.79
DETECT     = 5.45 ms        采样 4.96 + 跟踪 0.47
                            帧预算 12.5ms，占用 44%
UART       = 50 Hz          400 帧/8 秒，errors=0
GREEN_STAT = hits=0         （画面无目标，无误报）
```

对比阶段 0（纯单目标检测 5.30ms）：**新增开销 +0.15ms**，帧率无变化。

### 单元测试（不依赖板子）

```sh
wsl -e bash -c "cd /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget && \
  g++ -std=c++11 -O1 -Wall -Wextra -I src tests/test_phase1.cpp \
      src/target_track.cpp src/green_detect.cpp -o /tmp/t && /tmp/t"
```

覆盖 63 项：角度换算（含补偿/翻转/未标定）、卡尔曼收敛、状态机迁移、
多目标锁定与切换、长宽比过滤、时间戳/采集帧间隔/dt/板端年龄（含时间倒退与
无符号下溢保护），以及**瞄准点向上偏移**（offset=0/1.5、按框高缩放）。
**当前 63/63 通过。**

串口协议另有字节级自检（pty 冒充串口，跑生产代码 `uart_out.cpp`，不需要硬件）：

```sh
g++ -std=c++11 -O1 -Wall -Wextra -I src tests/uart_protocol_harness.cpp src/uart_out.cpp \
    -o /tmp/uart_harness -pthread && /tmp/uart_harness
# 期望：37 项全部通过，并打印真实帧字节（如 $GT,0,0.00,...,-1,0,0*02）
```

### ⚠️ 标定参数目前是占位值

`config.yaml` 里的 `cam_fx/fy/cx/cy` 是**按同型号相机估算的，不是本机标定结果**。
不标定也能跑，但角度有系统偏差，**不能用于闭环控制**。

标定步骤见 **[docs/CALIBRATION.md](docs/CALIBRATION.md)**（棋盘格，约 30 分钟），
填完 `config.yaml` 和 `config_boot.yaml` 后重启生效。

## 已知限制

- **帧率 80fps（1280×720）**：靠项目自带的生产版 GC4653 驱动实现（720P 寄存器表）。
  1440P 模式为 41.3fps。
- **`detect_sample_steps=1` 时覆盖率仅 1/16**：远处小目标可能漏检。
  提高密度会掉帧（ns=2 → 47fps），需改成两级检测（尚未实现）。
- **不做镜头畸变校正**：画面边缘角度偏大（本机 k1≈−0.45，边缘误差可达 2~3°）。
- **真实绿色目标的检出率尚未验证**：目前只验证了「无目标时无误报」（hits=0）。
  有效射程、最小可检像素数、光照适应性都需要实测。
- **长时间运行会热降频**：满负荷跑几分钟温度会升到 70°C+，实测降频后掉到 13~34fps。
  `start.sh` 有保护（70°C 禁止启动 / 74.5°C 停机），但赛场上需要物理散热。
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
build/bin/camera   467,440 字节   RISC-V musl（2026-11-08，含帧时间戳改动）
运行时依赖 12 个板端 .so，均存在于 /mnt/system/usr/lib
```

