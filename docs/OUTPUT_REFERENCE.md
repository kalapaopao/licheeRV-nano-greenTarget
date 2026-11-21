# 终端输出与数据文件 · 逐字段说明

> 这份文档回答两个问题：**终端每行是什么**、**文件里每列是什么**。
> 所有字段都对着 `src/camera.cpp` / `src/frame_log.cpp` / `src/uart_out.cpp` 里的真实打印语句核对过，
> 不是凭记忆写的。板端实机样例见各节末尾的 `示例:`。

---

## 一、终端输出

### 1. 启动阶段（只打一次，`print_boot` 控制）

```
RUN_CONFIG mode=manual exposure_us=3000 gain=1024,1024,1024 blc=manual jpeg=85 duration=0 recording=1 record_enable=true display_enable=true http_enable=false
```

| 字段 | 含义 |
|---|---|
| `mode` | `manual` / `auto`，曝光模式 |
| `exposure_us` | 手动曝光时间（微秒） |
| `gain` | 模拟增益, 数字增益, ISP 增益（都是 ×1024，1024 = 1.0×） |
| `blc` | 黑电平模式 |
| `jpeg` | JPEG 质量（75~90） |
| `duration` | 计划运行秒数（0 = 常驻） |
| `recording` | 1 = 会往磁盘写数据 |
| `record_enable` / `display_enable` / `http_enable` | 对应配置项的值 |

```
UART1_PINMUX A18:0x00000004->0x00000006 A19:0x00000004->0x00000006 (6=UART1 RX/TX)
```

启动脚本把 UART1 的数据脚从 A28/A29 挪到 A18/A19（原因见 `UART_PROTOCOL.md` §1.1）。
只有值不是 6 时才打印，已经是 6 就不打。

```
UART: /dev/ttyS1 @ 115200 8N1, rate=50Hz, format=binary-v2(28B), rx_print=on(10Hz), tx=on
```

| 字段 | 含义 |
|---|---|
| `/dev/ttyS1 @ 115200 8N1` | 串口设备与格式 |
| `rate=50Hz` | 视觉→电控的发送频率 |
| `format=binary-v2(28B)` | 发送帧格式：28 字节二进制 v2 |
| `rx_print=on(10Hz)` | 收到电控帧后按 10Hz 打印（电控实际 100Hz，全打会刷屏） |
| `tx=on` | 是否真的往串口写 |

```
CAMERA_READY display=on http=off preview=none recorder=on
```

| 字段 | 含义 |
|---|---|
| `display` | 是否在本地屏幕/预览上显示 |
| `http` | 是否开了 HTTP 推流服务（`http_enable`） |
| `preview` | 预览目标（关掉时是 `none`） |
| `recorder` | 是否在录像 |

```
RECORDING_ARMED output_dir=/code/fly_LicheeRV/record format=AVI/MJPEG (等待起飞触发才写文件)
WEB_DISABLED (http_enable=false display_enable=true)
```

- `RECORDING_ARMED` = 已经开始**预录缓冲**，但磁盘上一个文件还没有（等触发才写）。
- 没开记录时打的是 `RECORDING_OFF`。
- `WEB_READY ip=...` / `WEB_DISABLED` 二选一。

```
EXPOSURE_SET us=3000 again=1024 dgain=1024 isp_gain=1024 rc=0
RECORD80_AE_INFO elapsed_s=5.41 rc=0x0 exp_us=10000 again=16640 dgain=1024 isp_gain=1032 luma=27
AE_EXP_CLAMP max=100000 -> 10000（80fps 帧周期只有 12.5ms）
```

| 字段 | 含义 |
|---|---|
| `EXPOSURE_SET` | 手动模式下**我们设进去**的值；`rc` 非 0 = 没设成功 |
| `AE_INFO` | 每 5 秒**回读** ISP 当前实际生效的值 |
| `exp_us` | 当前曝光时间（微秒） |
| `again` / `dgain` / `isp_gain` | 模拟/数字/ISP 增益（×1024） |
| `luma` | ISP 统计到的画面平均亮度（0~255，正常 40~60，`27` 偏暗） |
| `AE_EXP_CLAMP` | auto 模式下 AE 想要更长的曝光，被我们的 10ms 上限夹住 |

> `RECORD80_` 前缀是历史遗留（早期程序名），不代表功能。

### 2. 运行中 · 每 5 秒一组性能统计（`print_perf`）

```
capture_frames=1266 host_fps=79.0573 pts_fps=79.0628 duplicate_pts=0 timeouts=0 encoded=1263 encode_errors=0 encode_fps=76.982 sent_fps=0.000 jpeg_mbit_s=0.000 preview_replaced=2
```

| 字段 | 含义 | 怎么看 |
|---|---|---|
| `capture_frames` | 程序启动以来的**累计取帧数** | 只增 |
| `host_fps` | **按主机时钟**算的实际帧率 | 目标 80；掉到 60 多说明曝光太长或 CPU 忙 |
| `pts_fps` | **按 VI 的 PTS** 算的帧率 | 与 `host_fps` 差异大 = 硬件时间戳有跳变 |
| `duplicate_pts` | 相邻帧 PTS 重复的次数 | 应该 0；不为 0 说明驱动时间戳有问题 |
| `timeouts` | `CVI_VI_GetChnFrame` 超时的累计次数 | 应该 0；涨说明取帧跟不上 |
| `encoded` | 累计编码出的 JPEG 帧数 | 与 `capture_frames` 差太多 = 丢帧 |
| `encode_errors` | 编码失败累计次数 | 应该 0 |
| `encode_fps` | 这 5 秒的编码帧率 | 低于 `host_fps` 说明编码跟不上 |
| `sent_fps` | 推流给浏览器的帧率 | 没开 HTTP 时恒 0 |
| `jpeg_mbit_s` | 推流带宽（Mbit/s） | 没开 HTTP 时恒 0 |
| `preview_replaced` | 主线程**覆盖掉还没来得及编码的帧**的次数 | **不为 0 就是视频在丢帧** |

```
DETECT avg_ms=5.520 max_ms=13.300 proc_avg_ms=6.100 proc_max_ms=14.200 lat_avg_ms=7.800 lat_max_ms=19.190 calls=1200 stride=1 map_miss=0 sample_ms=2.100 track_ms=3.400 ns=1 cand_max=3
```

| 字段 | 含义 |
|---|---|
| `avg_ms` / `max_ms` | 这 5 秒内**检测本身**（抽点+找绿+筛选）的平均/最大耗时 |
| `proc_avg_ms` / `proc_max_ms` | 检测 + 后处理（卡尔曼更新等）的耗时 |
| `lat_avg_ms` / `lat_max_ms` | **取帧完成 → 结果算出**的总延迟，最贴近"反应快不快" |
| `calls` | 这 5 秒跑了多少次检测 |
| `stride` | 抽帧步长（`detect_stride`，1 = 每帧都检测） |
| `map_miss` | 虚拟地址映射失败的次数（应该 0） |
| `sample_ms` | 抽点下采样耗时 |
| `track_ms` | 多目标筛选 + 跟踪耗时 |
| `ns` | 本帧候选/跟踪的目标个数 |
| `cand_max` | 这 5 秒内单帧最多检出几个候选 |

```
PTS_STAT last_raw=123456789 window_delta_raw=12500000 per_frame_raw=200000.0 duplicate_pts=0
CLOCK36_WINDOW elapsed_s=5.00 frames=399 host_fps=79.80 pts_fps=79.81
```

- `PTS_STAT`：VI 原始 PTS 的窗口增量，用来判断硬件时钟是否稳定。
  80fps 时 `per_frame_raw` 应该 ≈ 200000（微秒单位）。
- `CLOCK36_WINDOW`：按窗口重新计算的帧率，和 `host_fps` 对得上就说明两者一致。

```
GREEN_STAT frames=1200 hits=980 rate=81.7%
```

| 字段 | 含义 |
|---|---|
| `frames` | 这 5 秒检测了多少帧 |
| `hits` | 其中有多少帧检出了绿色目标 |
| `rate` | 命中率 = hits / frames |

### 3. 电控下行数据（`print_mcu` 控制，10Hz 限频）

```
[MCU] seq=  5 state=FLIGHT flags=0x07(imu=1 bias=1 acc=1) roll=  26.90 pitch= -20.81 yaw=  -3.57 | gx=  78.46 gy= 196.55 gz=  62.42 | age=0ms
```

| 字段 | 含义 | 陷阱 |
|---|---|---|
| `seq` | 电控帧的序号（0~255 循环） | **不能用来对齐视觉**：协议 §4.5 说明 0x02 不带时间戳、也不回显我们的序号 |
| `state` | `STATIC`(0) / `FLIGHT`(1) / `STOP`(2) | **录像就是靠 STATIC→FLIGHT 这个边沿触发的** |
| `flags` | 位标志，括号里是解出来的三位 | `imu=0` 时下面 6 个 IMU 值是电控发的 0.0，**不是真的零姿态** |
| `roll/pitch/yaw` | 电控姿态角（度） | |
| `gx/gy/gz` | 三轴角速度（度/秒） | "飞镖怎么转"就看这三个 |
| `age` | 这一帧距离**现在**过了多少毫秒 | 100Hz 发送时正常 0~10ms；变大说明电控掉速或链路卡 |

`flags` 位定义：`bit0` IMU_VALID、`bit1` BIAS_READY、`bit2` ACC_VALID。

### 4. 目标检测明细（`print_target` 控制，10Hz 限频）

```
GREEN_TARGET state=confirmed yaw=12.34 pitch=5.67 vyaw=45.20 vpitch=-12.30 dist=3.456 age=8.0ms cand=2 px=640.0,360.0 r=45.5 lyaw=12.00 lpitch=5.50 hits=980 frame=1266
```

| 字段 | 含义 |
|---|---|
| `state` | `lost`(丢失) / `coasting`(滑行，短暂漏检但还沿用预测) / `confirmed`(已确认) |
| `yaw` / `pitch` | 目标相对光轴的角度（度，正=右 / 正=上），**已加瞄准点偏移** |
| `vyaw` / `vpitch` | 角速度（度/秒），给电控做前馈 |
| `dist` | 单目测距（米）；后面带 `(invalid)` 表示不可信 |
| `age` | 距最近一次**有效测量**过了多少毫秒（跟踪器的新鲜度，不是电控的 age） |
| `cand` | 本帧候选目标个数 |
| `px` | 目标中心在**原图**上的像素坐标 |
| `r` | 目标等效半径（像素） |
| `lyaw` / `lpitch` | 引导灯中心角度（**未加**瞄准点偏移，联调对照用） |
| `hits` | 累计命中帧数 |
| `frame` | 相机帧号（对应 `frames.csv` 的 `frame_id`） |

### 5. 串口统计（每 5 秒，`print_uart` 控制）

```
UART sent=1516 rate=50.08Hz errors=0 | rx_frames=3030 rx_crc=0 rx_other=0 rx_age=2ms
```

| 字段 | 含义 |
|---|---|
| `sent` | 累计发出的**帧数**（不是字节数） |
| `rate` | 实际发送频率（目标 50Hz） |
| `errors` | 写失败次数（应该 0） |
| `rx_frames` | 累计收到并解析成功的电控帧数 |
| `rx_crc` | CRC 校验失败次数（应该 0；涨 = 线路干扰或波特率不对） |
| `rx_other` | 收到但不是 `CMD=0x02` 的帧数（自环测试时会涨，正常应为 0） |
| `rx_age` | 最近一帧距今毫秒；`-1` = 还没收到过 |

### 6. 记录/会话事件（`print_log` 控制）

```
LOG_SESSION_OPEN dir=/code/fly_LicheeRV/record/sess_272s_3960_001 trigger_us=272468911
VIDEO_PREROLL flushed=216 帧
LOG_SESSION_CLOSE dir=/code/fly_LicheeRV/record/sess_272s_3960_001 reason=landed frames=1099 mcu=0 dropped=0
```

| 字段 | 含义 |
|---|---|
| `sess_<uptime>s_<pid>_<n>` | 会话目录名：开机第几秒 + 进程号 + 第几次会话 |
| `trigger_us` | 触发时刻（单调时钟微秒） |
| `VIDEO_PREROLL flushed` | 会话一打开，先把内存里缓冲的**预录视频帧**写进 AVI 的帧数 |
| `reason` | 关闭原因：`landed`(落地) / `max_seconds`(到时长上限) / `stopped`(被停) / … |
| `frames` | 这次会话写进 `frames.csv` 的行数 |
| `mcu` | 独立 `mcu.csv` 的行数（`log_mcu_stream: false` 时是 0） |
| `dropped` | 会话进行中因缓冲满而丢的行数 |

### 7. 退出阶段

```
EXIT_AFTER_LAND sessions=1 会话已归档，进程退出
SHUTDOWN step=1 encoder_join begin
SHUTDOWN step=2 uart_stop begin (+0ms)
...
stopped frames=1076 encoded=1039 errors=0 tuning_failed=0 recording_failed=0
```

| 字段 | 含义 |
|---|---|
| `EXIT_AFTER_LAND` | `exit_after_land: true` 时，落地归档完主动退出 |
| `SHUTDOWN step=N ... (+Xms)` | 收尾各步打点，`+Xms` 是相对收尾开始的累计耗时。卡在哪一步一眼能看出来 |
| `stopped frames=` | 全程总共取了多少帧 |
| `encoded` | 总共编码了多少帧 |
| `errors` | 编码错误累计 |
| `tuning_failed` | 1 = ISP 参数设置失败（曝光/黑电平等） |
| `recording_failed` | 1 = 录像写盘失败过 |

---

## 二、磁盘上的文件

一次飞行的产物都放在 `record/sess_<...>/` 里；**没触发时 `record/` 是空的**（预录只在内存里）。

```
record/sess_272s_3960_001/
  frames.csv         一帧一行，33 列（主表）
  video.avi          MJPEG 视频
  video_frames.csv   AVI 帧号 ↔ 相机帧号 映射
  health.csv         5 秒一条的健康记录
  meta.txt           这次会话的元信息
  mcu.csv            仅在 log_mcu_stream: true 时才有（100Hz 原始电控流）
```

### 2.1 `frames.csv`（33 列，80Hz，一帧一行）

| 列 | 含义 | 备注 |
|---|---|---|
| `frame_id` | 相机帧号，每取到一帧 +1 | **跳号 = 那几帧没进记录**（取帧超时） |
| `capture_us` | **取帧时刻**（`CLOCK_MONOTONIC` 微秒） | ⚠️ 不是曝光时刻 |
| `pts_raw` | VI 驱动给的原始 PTS | 只记录，不换算 |
| `ran_detect` | 1 = 这帧跑了检测；0 = stride 跳过 / 检测关闭 | `ran_detect=0` 时后面检测列是默认值 |
| `detect_us` | 抽点→检测完成的耗时（微秒） | |
| `lat_us` | 取帧完成→结果算完的耗时（微秒） | 最贴近"反应延迟" |
| `detected` | 1 = 本帧对外报"有目标" | |
| `state` | 跟踪状态：0 lost / 1 coasting / 2 confirmed | |
| `seq` | 视觉跟踪器自己的递增序号 | 每次状态更新 +1 |
| `age_ms` | 距最近一次有效测量的毫秒数 | 跟踪器新鲜度 |
| `cand` | 本帧候选目标数 | |
| `yaw_deg` | 目标水平角（度，正=右） | 已加瞄准点偏移 |
| `pitch_deg` | 目标俯仰角（度，正=上） | 已加瞄准点偏移 |
| `vyaw_dps` | 水平角速度（度/秒） | 前馈用 |
| `vpitch_dps` | 俯仰角速度（度/秒） | 前馈用 |
| `dist_m` | 单目测距（米） | 需标定 `target_diameter_m` |
| `dist_valid` | 1 = `dist_m` 可信 | 0 时 `dist_m` 无意义 |
| `light_yaw_deg` | 引导灯中心水平角（度） | **未加**偏移，对照用 |
| `light_pitch_deg` | 引导灯中心俯仰角（度） | **未加**偏移 |
| `gx_px` / `gy_px` | 目标中心在**原图**上的像素坐标 | 标定完善后可用它重算角度 |
| `gr_px` | 目标等效半径（像素） | 可估距离（像素越小越远） |
| `has_box` | 1 = 画了框 | |
| `mcu_state` | 取帧那一刻的电控 STATE（0/1/2） | 无数据时 `nan` |
| `mcu_flags` | 取帧那一刻的 FLAGS | 无数据时 `nan` |
| `mcu_age_ms` | 该帧采集时刻 − 电控数据接收时刻（毫秒） | 越小越"新鲜"，正常 0~25 |
| `mcu_rx_us` | 该电控帧的**绝对接收时刻**（微秒） | 无数据时 `nan` |
| `mcu_roll` / `mcu_pitch` / `mcu_yaw` | 电控姿态角（度） | |
| `mcu_gx` / `mcu_gy` / `mcu_gz` | 电控三轴角速度（度/秒） | "怎么转"看这三个 |

> **一行里有两个时刻**：画面 = `capture_us`，电控数据 = `mcu_rx_us`，二者之差就是 `mcu_age_ms`。
> 跨流对齐只按时间戳做 join，不要按 `seq`——三路频率本来就不一样（视觉 50Hz / 电控 100Hz / 检测 80Hz）。
>
> 没收到电控数据时，最后 11 列全部写 `nan`，**列数恒定为 33**，pandas/Excel 按缺失值处理。

### 2.2 `video_frames.csv`（4 列）

| 列 | 含义 |
|---|---|
| `avi_index` | 该帧在 `video.avi` 里的序号（0 起） |
| `frame_id` | 对应的相机帧号 |
| `capture_us` | 该帧的取帧时刻 |
| `pts_raw` | 该帧的 VI 原始 PTS |

用途：**在视频里定位 CSV 的任何一行**。想找"第 500 帧之前 1 秒"的画面，就查 `capture_us` 最接近的那行，拿到 `avi_index` 再去 AVI 里跳。

### 2.3 `health.csv`（8 列，5 秒一条）

| 列 | 含义 |
|---|---|
| `mono_us` | 这一条的时刻（单调时钟微秒） |
| `fps` | 当时的帧率 |
| `frames` | 累计取帧数 |
| `encoded` | 累计编码帧数 |
| `mcu_rx` | 累计收到的电控帧数 |
| `mcu_age_ms` | 当时电控数据的年龄 |
| `session_open` | 1 = 会话正在进行 |
| `dropped` | 累计丢弃的行数 |

用途：**看整场运行的健康曲线**——帧率有没有掉、电控有没有断、记录有没有丢。

### 2.4 `mcu.csv`（10 列，仅 `log_mcu_stream: true`）

`rx_us, seq, state, flags, roll, pitch, yaw, gx, gy, gz`，按电控原始 100Hz 记录。

⚠️ `flags & 0x01 == 0` 时六个 IMU 字段是**电控发的 0.0，不是真的零姿态**，分析前先按 `flags` 过滤。

### 2.5 `meta.txt`（会话元信息）

| 键 | 含义 |
|---|---|
| `session` | 第几次会话 |
| `wall` | 墙上时间（板子无 RTC，1970 起算，仅供粗略参考） |
| `uptime_s` | 开机到触发经过的秒数 |
| `trigger_mono_us` | 触发时刻（单调微秒） |
| `mode` | `trigger`（触发式）/ `continuous` |
| `preroll_ms` / `postroll_ms` | 触发前/落地后各保留多久 |
| `max_seconds` | 单段最长秒数 |
| `mcu_stream` | 是否另存了 `mcu.csv` |
| `build` | 编译时间戳 |
| `cfg ...` | 启动时的关键配置快照（排查"这次数据为什么是这样"先看它） |
| `close_reason` | 关闭原因：`landed` / `max_seconds` / `stopped` |
| `close_mono_us` | 关闭时刻 |
| `land_mono_us` | 收到落地（STOP）的时刻 |
| `rows_frames` | `frames.csv` 行数 |
| `rows_mcu` | `mcu.csv` 行数 |
| `dropped` | 会话中丢弃的行数 |
| `video` | 视频文件名 |
| `video_frames` | `video.avi` 的帧数 |
| `video_first_avi` / `video_last_avi` | 该段视频在 AVI 里的首/末帧号 |

### 2.6 `video.avi`

MJPEG 编码的 AVI。时间基准用帧的 PTS，所以**播放速度 = 真实速度**。
视频**包含预录段**（触发前 `preroll_ms` 内的画面），起点等于 `video_frames.csv` 第一行的 `capture_us`。

### 2.7 `state/run_history.log`（不在会话目录里）

每次启动/退出/异常都追加一条，用于"程序怎么没的"归因：

```
t=1234s wall=... pid=3723 tag=supervisor_exit reason=landed_exit rc=0 duration=120
```

常见 `tag`：`supervisor_start` / `ready` / `alive` / `temp_limit` / `supervisor_exit` / `exit`。
常见 `reason`：`normal` / `test_completed` / `landed_exit` / `camera_crash` / `stopped_by_signal` / `temp_limit` / `clock_changed`。

---

## 三、最常见的误读

| 现象 | 不是问题的情况 | 是问题的情况 |
|---|---|---|
| `frame_id` 跳号 | 偶尔跳 1~2（取帧超时） | 连续大段跳号 → 相机被抢 CPU |
| `preview_replaced` > 0 | — | **一定是视频在丢帧**，检查 JPEG 体积/写卡速度 |
| `encode_fps` < `host_fps` | 短暂低一点 | 持续低一大截 → 同上 |
| `mcu_age_ms` 偶尔 20+ | 电控 100Hz，间隔 10ms，20ms 内正常 | 持续 >50ms → 串口或电控掉速 |
| `flags` 里 `imu=0` | 电控还没校准完 | 全程为 0 → 电控 IMU 没起来 |
| `dist` 带 `(invalid)` | 目标太小或未标定 | 一直 invalid → 标定参数不对 |
| `luma` 30 左右 | 暗环境（会导致增益变高、噪点多） | 长期 <20 或 >200 → 曝光策略有问题 |
