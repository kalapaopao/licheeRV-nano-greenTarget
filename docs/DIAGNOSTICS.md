# 运行状态 / 退出归因 / 打印开关

> 实现：`src/diag.{h,cpp}`（开关 + run_history + 崩溃归因）、`start.sh`（监督进程也写同一份历史）、
> `status.sh`（一眼看板端在干什么）。数据记录部分见 [DATA_RECORDING.md](DATA_RECORDING.md)。

## 1. 它解决什么问题

开机自启动、没有终端、板子还可能摔/掉电。三件事必须还能回答：

| 问题 | 现在谁回答 |
|---|---|
| 这次开机到底跑起来没有？ | `state/run_history.log` 的 `tag=ready` 行 + `status.sh` |
| 跑着跑着为什么停了？ | `tag=exit` / `tag=crash` / `tag=supervisor_exit` 行（带原因、帧数、温度） |
| 现在在干什么、热不热？ | `sh status.sh`（进程/温度/时钟/最近日志/最近记录） |

## 2. `state/run_history.log` 长什么样

追加式，一行一件事，跨重启保留（超过 256 KB 自动轮转成 `.1`，保留上一份）：

```
t=2229s wall=1970-01-01T00:37:09 pid=2553 tag=supervisor_start config=.../config.yaml duration=0 display=true record=0 uart=false/true log=false/trigger print=true
t=2229s wall=1970-01-01T00:37:09 pid=3445 tag=start print_mask=0xFD log=0 uart=0 dev=/dev/ttyS1 fmt=binary
t=2230s wall=1970-01-01T00:37:10 pid=3445 tag=ready display=1 record=0 uart=0 log=0 temp=69C web=0.0.0.0
t=2234s wall=1970-01-01T00:37:14 pid=3445 tag=alive frames=1 fps=0.00 encoded=0 temp=70C mcu_age=-1ms log_open=-1
t=2264s wall=1970-01-01T00:37:44 pid=3445 tag=alive frames=2430 fps=80.97 encoded=2428 temp=72C mcu_age=-1ms log_open=-1
t=2273s pid=3445 tag=crash sig=11 frames=2430 encoded=2428 temp=72C
```

| tag | 谁写 | 含义 |
|---|---|---|
| `supervisor_start` | start.sh | 监督进程拉起（含配置路径、时长、各开关状态） |
| `start` | camera | 配置解析完、串口/记录器就绪前的快照 |
| `ready` | camera | VI/ISP/VENC 全部就绪 = **"这次真的起来了"** |
| `alive` | camera | 每 30 秒一条心跳：帧数/帧率/温度/电控年龄 |
| `exit` | camera | 正常退出：`rc=`、`reason=`、帧数、错误数、信号、温度 |
| `crash` | 崩溃处理器 | 段错误等致命信号：`sig=`、崩溃瞬间的帧数/温度 |
| `temp_limit` | start.sh | 温度保护停机（带毫摄氏度原始值） |
| `supervisor_exit` | start.sh | 监督进程退出：`reason=camera_crash/temp_limit/test_completed/hangup/...` |

**怎么看一次故障**：`tail -30 state/run_history.log`。典型形态——

- 只有 `supervisor_start` 没有 `start`：camera 没起来（配置错误/被 `TOO_HOT_TO_START` 拦住）。
- 有 `start` 没 `ready`：起来一半就死了，问题在 VI/ISP/VENC 初始化（看日志里的 `STEP ... rc=`）。
- 有 `ready` 然后 `alive` 断了、也没有 `exit`：**异常终止**（掉电、SIGKILL、或崩溃处理器来不及写）。
- `exit reason=stopped_by_signal stop_signal=15`：被 `stop.sh` 或 `PR_SET_PDEATHSIG` 正常收掉。

## 3. 打印开关（`config.yaml`）

所有终端输出（`start.sh` 重定向到 `/tmp/fly_licheerv_capture.log`）按类开关，默认全开 = 与之前完全一样：

| 键 | 默认 | 关掉后消失的行 |
|---|---|---|
| `print_enable` | `true` | **总开关**：`false` 时除错误行和退出摘要外全静音（历史照写） |
| `print_boot` | `true` | `TRACK calib` / `POOL_SIZE` / `SENSOR_MODE` / `CAMERA_READY` / `VI geometry` … |
| `print_isp` | `true` | `RECORD80_AE_*` / `EXPOSURE_SET` / `RECORD80_BLC_*` / `DIAG ISP/TNR/DPC` |
| `print_target` | `true` | `GREEN_TARGET`（10Hz 检出行）、`GREEN_STAT`（每 300 帧） |
| `print_perf` | `true` | `capture_frames=` / `DETECT avg_ms=` / `TIMING` / `PTS_STAT` / `CLOCK36_WINDOW` |
| `print_uart` | `true` | `UART: ...` 启动行、`UART sent=...` 统计 |
| `print_mcu` | `true` | `[MCU] ...` 电控下行帧 |
| `print_log` | `true` | `LOG:` / `LOG_SESSION_OPEN` / `LOG_SESSION_CLOSE` / `LOG sessions=` |
| `print_avi` | `true` | `RECORDING_STARTED` / `RECORDING_SAVED` |

**永远打印、不受开关影响**：`CONFIG_ERROR`、`Invalid ...`、`UART write failed`、
`VENC encode rc=`、`TEMP_LIMIT=`、`stopped ...`。
`state/run_history.log` 里的 `exit`/`crash` 行**文件永远写**（stdout 那一份受 `print_boot` 控制）。
理由：排障时最怕的就是"把线索关掉了"。

开机自启场景建议 `print_isp: false`（ISP 回读是纯噪音），其余全开。

## 4. `sh status.sh`

一条命令看全貌（只读，不改状态）：

```
== 系统 ==      host / uptime / load / 温度 / 时钟寄存器（0x00210001=36MHz 加速态）
== 进程 ==      camera 是否在跑、pid、RSS；supervisor 是否在跑
== 最近日志 ==  tail -8 /tmp/fly_licheerv_capture.log
== 运行历史 ==  tail -20 state/run_history.log
== 最近记录 ==  最近一个 record/sess_*/ 的 meta（触发时刻/结束原因/行数）
== 配置摘要 ==  uart/log/print/display/record 的关键开关
```

## 5. 崩溃归因是怎么做的

`diag_install_crash_handlers()` 注册 `SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT`：

1. 处理器里**只用 `open/write/close` 和一个手写整数转换**（`printf/snprintf` 在信号处理器里不是
   异步信号安全的，用了可能二次崩溃、什么也留不下）；
2. 写出 `tag=crash sig=<n> frames=<n> encoded=<n> temp=<n>C`，帧数/温度来自 5 秒一次的
   `diag_progress()` 快照；
3. 恢复默认动作并 `raise(sig)`，进程以"被信号杀死"的状态退出——所以 `start.sh` 的
   `kill -0` 检测能区分"崩溃"和"正常跑完"，并写 `supervisor_exit reason=camera_crash`。

**板上实测**（2026-11-19，对运行中的 camera 直接 `kill -SEGV`）：

```
t=2273s pid=3445 tag=crash sig=11 frames=2430 encoded=2428 temp=72C
```

## 6. 孤儿保护（防止"没人看管的相机"）

`camera` 启动时 `prctl(PR_SET_PDEATHSIG, SIGTERM)`：监督进程（`start.sh`）一旦消失，
相机收到 SIGTERM 后走正常退出路径并写 `tag=exit reason=stopped_by_signal stop_signal=15`。

为什么需要：实测过一次 `AUTO_BOOT=1 sh start.sh &` 被 ssh 断线带走的场景——监督进程没了、
相机还在满负荷跑，**温度保护和重启逻辑同时失效**，这是最危险的状态。

**板上实测**（2026-11-19）：`kill -9` 掉监督进程后，相机在约 5 秒内自己退出，并写下

```
t=2908s pid=3190 tag=exit rc=0 reason=stopped_by_signal frames=605 errors=0 stop_signal=15 temp=71C
```

> 注意"约 5 秒"：SIGTERM 到达后还要走完 VI/ISP 的销毁流程，退出不是瞬时的。
> 判断孤儿保护是否生效，别只看 kill 后第 1 秒。

## 7. 已知限制

1. 板子没有 RTC，`wall=` 是 1970 年；跨机对齐只能靠 `t=`（开机秒数）和 `logs/*/meta.txt`。
2. `alive` 心跳 30 秒一条，所以"进程消失"的时间分辨率是 30 秒（崩溃处理器不受此限制）。
3. `print_enable: false` 只静音**我们自己写的**输出；SDK sample 层（`[SAMPLE_COMM_*]`、
   ISP 二进制的 banner）不受控制——那是第三方库直接往 stdout 打的。
4. 崩溃处理器不产生 backtrace（musl 没有可移植的 `backtrace_symbols`），只有信号号 + 现场快照。
   `sig=11` + 帧数/温度 + 日志最后几行足够定位"是哪一类问题"，但不足以定位到行号。

## 8. 验证矩阵（2026-11-19）

| 项 | 方式 | 结果 |
|---|---|---|
| 打印开关 | `tests/diag_test.cpp` | 15 项 0 失败（含掩码、历史文件、真实 SIGSEGV） |
| 崩溃归因 | 板端 `kill -SEGV` | `tag=crash sig=11` + `supervisor_exit reason=camera_crash rc=6` |
| 启动链 | 板端 `status.sh` | `supervisor_start → start → ready → alive` 齐全 |
| 开关生效 | 板端 `print_perf: false` | `print_mask=0xF7`；`capture_frames/DETECT/TIMING/CLOCK36` 全为 0 行，`GREEN_STAT/CAMERA_READY/VI geometry` 仍在 |
| 孤儿保护 | 板端 `kill -9` 监督进程 | 相机 ~5 s 后自退，`tag=exit stop_signal=15` |
| 正常停机 | 板端跑满 30 s | `supervisor_exit reason=test_completed rc=0` + 相机 `stop_signal=15` |
| 回归 | 主机 7 套测试 | 177 项 0 失败（记录器 31 / 诊断 15 / 串口 42+37+23+16 / 触发链路 13） |
