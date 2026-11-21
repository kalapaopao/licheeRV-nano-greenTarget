# licheeRV-nano-greenTarget

LicheeRV Nano（SG2002 + GC4653）上的**镖体视觉**：1280×720 @80fps 取流 → 绿灯检测 →
多目标筛选 + 卡尔曼 → 角度/角速度经串口发给电控，同时把飞行数据逐帧记成 CSV，
并且能在没有终端的情况下回答「这次开机跑没跑起来、为什么停了」。

- 连接与预览（USB/Wi-Fi 地址）：[docs/NETWORK_CONNECTION_GUIDE.md](docs/NETWORK_CONNECTION_GUIDE.md)
- 全部文档索引：[docs/README.md](docs/README.md)

---

## 1. 三条命令上手

```powershell
# 编译（WSL 当执行器，源码和产物都在 Windows 盘上）
wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/build.sh
# 产物：build/bin/camera（RISC-V musl，约 500 KB）
```

```powershell
# 部署
$key="$env:USERPROFILE\.ssh\licheepi_root_ed25519"
scp -i $key build\bin\camera root@10.133.124.1:/code/fly_LicheeRV/camera
scp -i $key start.sh status.sh root@10.133.124.1:/code/fly_LicheeRV/
scp -i $key config.yaml root@10.133.124.1:/code/fly_LicheeRV/
```

```sh
# 板端
cd /code/fly_LicheeRV
sh start.sh --check-config     # 先校验配置（新键写错会在这里报错）
sh start.sh                    # 按 config.yaml 的 run_seconds 跑；0 = 常驻
sh status.sh                   # 另一窗口：看进程/温度/最近日志/最近一次记录
sh stop.sh                     # 停止（勿用 kill -9，会残留 VPSS，需重启板子）
```

浏览器预览：`http://10.133.124.1:8081/`（或 `http://licheervnano-857c.local:8081/`）。

---

## 2. 一帧数据是怎么流动的

```
                        ┌────────────── 采集线程（80Hz，主循环）──────────────┐
  GC4653 ──VI──► NV12 帧 ──┬─► 抽点下采样(4×4) → 绿灯检测 → 多目标筛选+卡尔曼 → 角度
  (1280×720)               ├─► 画检测框（写回 Y 平面，网页/录像才看得到）
                           ├─► 发布结果快照 ──► 串口线程（50Hz）──► 电控（28 字节 v2 帧）
                           ├─► 每帧一行 ──────► 记录器（环形缓冲，起飞/落地时落盘 CSV）
                           └─► 最新一帧 ──────► 编码线程 ──► 硬件 JPEG ──┬─► HTTP 预览
                                                                        └─► AVI 录像
  电控 ──► 串口线程接收（100Hz，33 字节 0x02 帧）──► 记录器（带电控状态一起存）
```

**线程一共 5 个（按开关会少）**，互不阻塞：主循环只做"取帧→算→发快照/入队"，I/O 全在别的线程。

| # | 线程 | 什么时候有 | 职责 |
|---|---|---|---|
| 1 | 主线程（图中"生产者"） | 总是 | 取帧 → 检测/跟踪/角度 → 画框 → 发布 → 交编码 → 记录器入队 |
| 2 | 编码线程 | `display_enable: true` | 取最新帧 → 硬件 JPEG → 网页 / AVI |
| 3 | HTTP 服务线程 | `display_enable: true` | 接连接、分配客户端槽 |
| 4..n | HTTP 客户端线程（最多 4） | 每个浏览器连接 | 只服务该客户端的 MJPEG/抓图，每客户端限 20fps |
| 5 | 串口线程 | `uart_enable: true` | 50Hz 发目标帧 + 收电控 0x02 帧 |
| 6 | 记录器写线程 | `log_enable: true` | 环形缓冲 → 文件 write / fdatasync |

所以实际线程数：**全关 1 个；只开相机 5 个（主+编码+HTTP+串口+记录器）；4 个预览客户端全连满 9 个**。

---

## 3. 目录（每个文件一句话）

```
src/camera.cpp          主程序：参数解析 → 初始化 VI/ISP/VENC → 主循环 → 收尾
src/green_detect.*      绿灯检测（YUV 判据 + 连通域 + 形状过滤，纯 C++11，无第三方库）
src/target_track.*      多目标筛选 + 角度域卡尔曼 + 像素→角度 + 瞄准点偏移
src/uart_frame.*        串口协议 v2 的 28/33 字节定长帧 + CRC16（纯函数，主机可测）
src/uart_out.*          串口线程：发目标帧、收电控帧（可只收不发）
src/frame_log.*         飞行数据记录：预录环形缓冲 + 触发落盘（每帧一行 CSV）
src/frame_timing.h      帧时间戳结构（帧号/采集时刻/原始 PTS，header-only）
src/diag.*              诊断：分类打印开关 + run_history + 崩溃归因
src/avi_mjpeg.h         MJPEG/AVI 封装（复用同一份硬件 JPEG，不二次编码）
src/sensor/             用户态 GC4653 驱动（含 720P 寄存器表）
src/common/             厂商 sample 层（VI/ISP/VENC 的胶水，约 7000 行，只在报错时查）

tools/detect_cli.cpp    对单帧 I420 跑检测的离线命令行
tools/test_detect.cpp   主机端检测自测入口
tools/flight_merge.py   把 frames.csv（+可选 mcu.csv）合成「一帧一行」并出图、打印摘要
tools/fake_mcu_pty.py   没有电控时造 0x02 帧（pty），用来验证整条记录链路
tools/draw_mindmap.py   生成 docs/mindmap_status.png
tools/decode_v2.py      串口抓包解码（v2 帧）
tools/uart_accept.py    联调验收工具（USB-TTL 监听线上字节，实时打印 + PASS/FAIL）
tools/gc4653_calibration.py  标定取图/计算
tools/run_host_tests.sh 一键跑全部主机测试
tools/check_refactor.py 重构守门员：比较两份 C++ 的 token，确认"只改格式没改逻辑"
tools/check_doc_links.py 检查 Markdown 里的相对链接是否都还存在
tools/draw_architecture.py / render_drawio_png.py  生成 docs/architecture/ 下的架构图
tools/gen_flight_timeline.py  生成「一次飞行的时间线」流程图
tools/gen_row_life.py    生成「一行数据的一生」流程图（两个时刻是怎么钉进一行的）
tools/gen_frame_timeline.py  生成「两个数据流的时间轴」（100Hz 电控 vs 80Hz 相机怎么错位）
tools/gen_thread_dataflow.py 生成「线程间数据流与共享结构」（谁写谁读、锁保护哪一步）
tools/gen_architecture_diagram.py 在原图上补画线程/记录器/诊断分区（架构图生成）
tools/board/            板端运维脚本（随仓库维护，部署到 <root>/tools/board/，见该目录 README）
  ├─ board_uart_check.sh     串口链路四层诊断（内核计数/抓包/引脚电平/设备树）
  ├─ board_verify_no_files.sh 验收：待机时 record/ 必须为空
  ├─ board_verify_trigger.sh  验收：假电控触发后会话目录五件齐全
  ├─ board_check_session.sh   查会话开关原因（close_reason/行数/视频区间）
  └─ board_slim.sh            板端瘦身（删文档/旧数据/备份，保留 tools/）

tests/                  单元测试与仿真（见第 7 节）
status.sh               一条命令看板端状态（进程/温度/历史/最近记录）
start.sh / stop.sh      启停：YAML 校验、36MHz 时钟切换、相机锁、温度保护、状态历史
config.yaml             手动运行配置（每个键上方都有注释）
boot/S99flycamera       开机自启脚本（部署到板端 /etc/init.d/）
build.sh                交叉编译脚本
CMakeLists.txt          编译配置（源码清单 + 链接参数）
```

---

## 4. 配置速查（`config.yaml`）

每个键的详细说明都在文件内的注释里，这里只给分组：

| 组 | 键 | 作用 |
|---|---|---|
| 曝光/增益 | `exposure_mode` `manual_exposure_us` `manual_*_gain_x1024` `auto_*_max` | 手动固定曝光（赛场推荐）或自动 + 上限 |
| 画质 | `jpeg_quality`(75..90) `black_level_*` | 网页/录像画质与黑电平 |
| 运行 | `run_seconds`(0=常驻) `web_ip` `display_enable` `record_enable` | 时长、绑定网卡、是否开编码/预览、是否录像 |
| 检测 | `detect_enable` `detect_stride` `detect_sample_steps`(1..4) | 抽点密度；**80fps 必须用 steps=1** |
| 网页服务 | http_enable | alse = 不起 HTTP 服务（省 CPU/带宽），**录像与编码不受影响** |
| 落地退出 | `exit_after_land` | `true` = 一次飞行归档完（落地 + postroll）程序自己退出，不常驻待命；`false` = 常驻等下一次起飞 |
| 画面叠加 | `osd_enable` `osd_scale`(1..3) | 把电控状态烧进画面左上角（4 行：帧号+STATE+SEQ+年龄 / 陀螺 / 姿态 / 目标角），录像里也看得到 |
| 标定 | `cam_fx/fy/cx/cy` `optical_*_deg` `cam_invert_*` `target_diameter_m` `target_up_offset_factor` | 像素→角度、光轴补偿、单目测距、瞄准点偏移 |
| 跟踪 | `track_enable` `sel_*` `kf_*` | 多目标评分、确认/丢失帧数、卡尔曼噪声 |
| 串口 | `uart_enable` `uart_device` `uart_baud` `uart_rate_hz` `uart_format` `uart_rx_print*` `uart_tx_enable` | **UART1 = `/dev/ttyS1`**（`ttyS0` 是内核 console，不能用）；v2 二进制（出厂）/ ASCII、50Hz、可只收不发 |
| 记录 | `log_enable` `log_mode` `log_preroll_ms` `log_postroll_ms` `log_max_seconds` | 起飞触发、预录窗口、落地后继续记多久 |
| 诊断 | `print_enable` `print_boot/isp/target/perf/uart/mcu/log/avi` | 分类打印开关，默认全开 |

`start.sh` 对**未知键或非法值直接报错退出**（`sh start.sh --check-config` 可先自检），
所以脚本与二进制必须一起更新。

---

## 5. 运行时你会看到什么

**日志**（`start.sh` 重定向到 `/tmp/fly_licheerv_capture.log`）：

```
capture_frames=... host_fps=79.9 ...          ← 采集/编码/网络，每 5 秒
DETECT avg_ms=5.3 ...                         ← 检测耗时，每 5 秒
UART sent=... rate=50.0Hz ...                 ← 串口收发统计，每 5 秒
GREEN_TARGET state=confirmed yaw=... → 电控    ← 有目标时 ≤10Hz
[MCU] seq=.. state=FLIGHT ...                 ← 电控下行帧，≤10Hz
LOG sessions=1 open=1 rows_frames=...         ← 记录器状态
```

**运行状态历史**（`state/run_history.log`，跨重启保留，自启动排障就看它）：

```
tag=supervisor_start  →  tag=start  →  tag=ready  →  tag=alive（每 30s）
                                              └─► tag=exit reason=... / tag=crash sig=11
```

**数据**：`record/sess_*/`（每次起飞一个文件夹：`frames.csv` + `video.avi` + `video_frames.csv` + `meta.txt`）、`record/health_*.csv`。

细节与字段定义：[docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md)、[docs/DATA_RECORDING.md](docs/DATA_RECORDING.md)。

---

## 6. 编译与部署细节

### 6.0 开发约定（2026-11-21 起）

| 约定 | 说明 |
|---|---|
| **不再生成 `*.bak` 备份** | 回退一律用 git：`git diff` 看改动、`git checkout -- <file>` 退回、`git revert` 撤销提交 |
| **改动即提交、即推送** | 每个可验证的小改动：`git add <明确文件> && git commit && git push origin feat/green-detect` |
| **正式合并走 PR** | 主干 `main` 只接受 PR；功能分支 `feat/green-detect` 上直接提交推送 |
| **板端不手工留备份** | 板端 `backup/` 目录已清理；历史版本以 git 为准 |
| `.gitignore` 已覆盖 | `build/ record/ logs/ state/ *.avi *.bak backup/`（数据与产物不入库） |

依赖（都在 `E:\study\LicheeRV\env\`，`build.sh` 已设好默认路径）：

| 目录 | 内容 | 需要吗 |
|---|---|---|
| `toolchain/` | RISC-V musl 交叉工具链（Xuantie-900 gcc 10.2.0） | 必需 |
| `sipeed-sdk-build/include` + `kernel_include` | SDK 头文件 | 必需 |
| `sipeed-sdk-build/lib_dyn` | 17 个板端 .so（链接期与运行期都用） | 必需 |
| `sipeed-sdk-build/lib` | 自编 .a（最终链接只用 `libini.a`） | 部分 |

- 链接时**显式 `-static-libstdc++ -static-libgcc`**：板端 `/usr/lib/libstdc++.so.6` 不是
  musl 工具链的产物，动态链上去会起不来。
- 传感器模式在代码里固定覆盖成 `720P_60FPS` 寄存器表 + 36MHz 时钟 → 实测 80fps；
  **不要改 `/mnt/data/sensor_cfg.ini`**。
- 为什么不用自编 .a 链接（会全绿画面）、WSL 里那 3.7 GB 源码树是干什么的：见
  [docs/HANDOFF_WSL_BUILD_ENV.md](docs/HANDOFF_WSL_BUILD_ENV.md) 与
  [docs/history/HANDOFF_ISSUE_720p_80fps.md](docs/history/HANDOFF_ISSUE_720p_80fps.md)。

---

## 7. 测试（不需要板子）

```powershell
wsl -e bash /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget/tools/run_host_tests.sh
```

覆盖 **240 项断言 + 2 个仿真**，全部通过（2026-11-19）：

| 测试 | 项数 | 内容 |
|---|---|---|
| `test_phase1` | 63 | 角度换算/卡尔曼收敛/状态机/多目标锁定/时间戳与 dt |
| `uart_frame_test` | 42 | v2 帧字节布局、CRC、边界 |
| `uart_protocol_harness` | 37 | ASCII 帧（pty 跑生产代码） |
| `uart_binary_pty_test` | 23 | 二进制帧端到端 |
| `uart_rx_pty_test` | 16 | 电控下行帧解析 |
| `frame_log_test` | 31 | 预录/裁剪/上限/空闲不空转（CPU 回归） |
| `log_trigger_pty_test` | 13 | 串口线程 → 记录器整条链路 |
| `diag_test` | 15 | 打印掩码、run_history、真实 SIGSEGV 归因 |
| `sim_kalman` / `sim_dart` | — | 跟踪仿真 |

---

## 8. 已知限制（交接必读）

- **帧率**：720P 80fps 靠项目自带驱动实现；1440P 模式 41.3fps。长时间满负荷会热降频。
- **散热**：实测满负荷几分钟就到 70°C+；`start.sh` 有 70°C 禁止启动 / 74.5°C 停机保护，
  赛场上必须加物理散热（或把停机改成降帧率——尚未实现）。
- **`detect_sample_steps=1` 覆盖率只有 1/16**：远处小目标可能漏检；调高会掉帧。
- **不做畸变校正**：画面边缘角度偏大（本机 k1≈−0.45，边缘误差可达 2~3°）。
- **内参仍是估算值**：`cam_fx/fy/cx/cy` 未标定，角度有系统偏差，**不能用于闭环**。
  标定步骤见 [docs/CALIBRATION.md](docs/CALIBRATION.md)。
- **真实目标检出率未验证**：目前只验证了"无目标时不误报"。
- **电控 `STATE` 的判定语义未确认**；`/dev/ttyS1` 是否引到排针需对原理图确认。
- 停止请用 `sh stop.sh`，**不要 `kill -9`**。

---

## 9. 代码怎么读

按这个顺序，一小时内能看懂主干：

1. **`src/frame_timing.h`**（90 行）——帧号/采集时刻是怎么定义的，后面所有"年龄"都基于它。
2. **`src/green_detect.h`** 的参数区 + `green_detect.cpp` 的 `detectFrame()`——检测主流程。
3. **`src/target_track.cpp`** 的 `TargetTracker::update()`——多目标怎么选、状态机怎么迁移。
4. **`src/uart_frame.cpp`**（220 行）——协议字节布局，纯函数，最好读。
5. **`src/camera.cpp`**：只看 7 个大节标题（0 命令行 / 1 参数 / 2 记录 / 3 诊断 / 4 启动外设 /
   5 初始化成像 / 6 主循环 / 7 收尾），主循环里按 ①~⑥ 的注释走一遍即可。
6. **`src/frame_log.cpp`**——记录器：环形缓冲怎么填、什么时候落盘、为什么不在采集线程写盘。

`src/common/`（约 7000 行）是厂商 sample 胶水，**只在初始化报错时查**，不必通读。

更细的逐段讲解见 [docs/READING_GUIDE.md](docs/READING_GUIDE.md)（其中的行号以 2026-11 版本为准，
代码结构看 `camera.cpp` 顶部的分节注释）。

---

## 10. 现状与下一步

已完成并验证：取流 80fps、检测 5.3ms/帧、角度输出、串口 v2 双向、飞行数据记录、
运行状态与退出归因、`status.sh`、240 项主机测试、板端 78.4–79.0fps（记录全开）。

还没做（按优先级）：

1. **电控联调**：`STATE` 判定语义、`ttyS1` 排针、线上字节对帧（阻塞项在对方/硬件）。
2. **相机标定**：内参 + 畸变，之后角度才有物理意义。
3. **散热策略**：把"超温停机"改成"超温降帧率"，避免赛场识别中断。
4. **真实目标验证**：有效射程、最小可检像素、光照适应性。
