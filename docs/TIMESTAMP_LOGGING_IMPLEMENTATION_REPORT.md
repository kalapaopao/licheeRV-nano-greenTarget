# 时间戳 / 跟踪 dt / 板端年龄 / 日志限频 —— 实施报告（给 Codex 复核）

> 对应执行文档：[docs/TIMESTAMP_LOGGING_IMPLEMENTATION_PLAN.md](TIMESTAMP_LOGGING_IMPLEMENTATION_PLAN.md)（下称「计划」）。
> 本文件记录**实际做了什么、与计划的差异、验证证据、未做项、复审重点**。
> 实施日期：2026-11-08。实施者：DSH（DeepSeek Harness）。项目：`E:/study/LicheeRV/licheeRV-nano-greenTarget`。

---

## 0. 一句话结论

计划的第一阶段**已实现并通过主机 54 项测试、riscv64 交叉编译、板端 4 场景实测**；
但**计划 §6 的「把 `age` 改成取帧→发送年龄」没有照做**——按用户决策改为「`age` 协议语义不变，
板端端到端年龄只进日志」（下称**方案 B**）。这是本次唯一与计划实质不符的地方，也是复审第一优先项。

第二轮按 Codex 复核意见做了两处修订（§11）：UART 年龄日志改名为 `uart_write_start_age_*`
（明确「写开始」口径）、`u64PTS` 单位表述在代码与文档中统一。**代码行为未变**，`age` 仍为方案 B。

---

## 1. 关键决策记录

### 1.1 用户决策：`age` 字段采用方案 B

**问题**：计划 §6 要求 `age = send_us - capture_mono_us`。但原 `age` 是「距最近一次**真实检测**的毫秒数」
（`src/target_track.cpp:278`），是电控唯一的「数据陈旧度」信号：coast 期（`det=1, state=1`，角度为卡尔曼外推）
它会持续增长，`docs/UART_PROTOCOL.md` §3.2 让电控用 `age > 200ms → 停止闭环`。
改成取帧→发送年龄后该值常态只有 12~20ms，**coast 期也永远"新鲜"**，电控那条保护永不触发 → 属于静默的接口语义破坏。

**用户选择**：方案 B —— `age` 保持原语义，端到端年龄只写日志。

| 方案 | 内容 | 结果 |
|---|---|---|
| A（计划原文） | `age = send - capture` | 放弃（会改电控依赖的语义） |
| **B（已实施）** | `age` 不变；发送时重算的端到端年龄进日志 | **采用** |
| C（备选，未采用） | `age = max(端到端年龄, 测量年龄)` | 保留两种信号，但需要重写协议说明；用户未选 |

**如果后续要改成 A 或 C**：只需改 `src/uart_out.cpp` 中 `format_ascii()` 的 `age` 取值来源
（现在是 `out.age_ms`，见 `src/uart_out.cpp:214`），并同步改 `docs/UART_PROTOCOL.md` 第二、三、六节；
`FrameTiming`/`UartSnapshot` 的管线已经就位，不需要再动 camera 侧。

### 1.2 其他实施取舍（低风险，已在代码注释写清）

1. 计划 §5 的 dt 片段抽成 `gt::capture_dt_s()`，**语义完全等价**（`prev==0` 或非递增 → 返回 fallback 0.0125s），
   目的是让计划 §9「时间测试」能在主机单测里覆盖生产代码，而不是测一份复制品。
2. 计划 §9 的 6 项时间测试写进既有 `tests/test_phase1.cpp` 的第 [6] 段，**没有**新建计划 §8 里
   「第二阶段可选」的 `tests/test_frame_timing.cpp` —— 少一个文件，仍用计划 §9 里那条编译命令。
3. 计划 §9 要求的 5 秒指标，用新增 `TIMING` / `PTS_STAT` 两行 + `DETECT` 行追加字段实现；
   `log_drop_count` 未打印（第一阶段没有异步日志队列，不存在丢弃）。
4. 计划第二阶段（`async_log.h/.cpp`、有界队列、日志线程、独立进程）**按计划要求未实现**。

---

## 2. 代码改动详解

### 2.1 新增 `src/frame_timing.h`（86 行，header-only）

API：

```cpp
namespace gt {
using MonoUs = uint64_t;

MonoUs monotonic_us();                                  // CLOCK_MONOTONIC 微秒

struct FrameTiming {
  uint32_t frame_id = 0;          // 相机帧号：每成功取到一帧 +1
  uint64_t vi_pts_raw = 0;        // VI 原始 PTS，只记录不参与计算
  MonoUs capture_mono_us = 0;     // 取帧成功时刻（不是曝光时刻）
  MonoUs detect_start_us = 0;     // 开始抽样/检测
  MonoUs detect_end_us = 0;       // 检测+跟踪+角度换算完成
  bool valid() const;             // frame_id!=0 && capture!=0
  uint32_t detect_duration_us() const;   // detect_start → detect_end
  uint32_t capture_latency_us() const;   // capture → detect_end
};

float capture_dt_s(MonoUs cur, MonoUs prev, float fallback);      // 两帧采集差（秒）
int64_t frame_age_us(bool has_timing, MonoUs capture, MonoUs now); // 无效 → -1
int frame_age_ms(bool has_timing, MonoUs capture, MonoUs now);     // 无效 → -1
}
```

语义约定（写进文件头注释）：
- `capture_mono_us` = 「程序拿到这一帧」的时刻，**不是曝光时刻**，曝光→出图延迟无法从这里的差值推算；
- `vi_pts_raw` 只保存/统计，单位确认前**不参与任何计算**（本次已实测确认单位，见 §4.3，但代码仍未使用）；
- `frame_id`（相机帧号，每帧 +1）与 `TargetOut::seq`（结果序号，跟踪器每次更新 +1）**是两套编号，不得混用**；
- 所有相减都先判 `now >= from`，**杜绝无符号下溢**（`frame_age_*` 返回 -1，`diff_us` 返回 0）。

### 2.2 `src/camera.cpp`（643 → 691 行）

| 位置 | 改动 |
|---|---|
| `:24` | `#include "frame_timing.h"` |
| `:39-49` | 新增文件级静态量：`g_capture_frame_id`、`g_last_track_capture_us`、`g_last_pub_capture_us`、`g_last_pub_frame_id`、`g_last_target_log_us`、`g_det_max_ms`、`g_proc_total_ms/g_proc_max_ms`、`g_lat_total_ms/g_lat_max_ms` |
| `:448-453` | `CVI_VI_GetChnFrame()` 成功后立刻建立 `gt::FrameTiming`（`frame_id`/`vi_pts_raw`/`capture_mono_us`） |
| `:532-533` | 进入检测块先记 `detect_start_us`（与既有 `td0` 同一时刻） |
| `:570-575` | dt 改为采集时刻差：`dt = gt::capture_dt_s(timing.capture_mono_us, g_last_track_capture_us, 0.0125f)`，随后更新基准 |
| `:578` | `Tracker::update()` 返回后立刻记 `detect_end_us`（不含画框/日志/发布） |
| `:618-629` | `GREEN_TARGET` 限频 10Hz（`log_now - g_last_target_log_us >= 100000`），日志尾部追加 `frame=%u` |
| `:633-635` | `g_uart.publish(tgt, timing)`，并记录最近发布帧的采集时刻/帧号 |
| `:636-642` | 三个耗时：`det_ms`（td0→发布，**原有 `avg_ms` 语义不变**，另加窗口 max）、`proc_ms`（抽样→结果）、`lat_ms`（取帧→结果） |
| `:659` | `DETECT` 行追加 `max_ms / proc_avg_ms / proc_max_ms / lat_avg_ms / lat_max_ms` |
| `:665-680` | 新增 `TIMING` 与 `PTS_STAT` 两行（如下） |

关键代码：

```cpp
// :448-453 —— 取帧成功立刻建立帧元数据（后面只传 POD 拷贝，绝不把 VIDEO_FRAME_INFO_S 交给异步线程）
gt::FrameTiming timing{};
timing.frame_id=++g_capture_frame_id;
timing.vi_pts_raw=frame.stVFrame.u64PTS;
timing.capture_mono_us=gt::monotonic_us();

// :570-575 —— dt 用采集时刻差（只有真正调用 Tracker::update 的帧才更新基准）
const float dt=gt::capture_dt_s(timing.capture_mono_us,g_last_track_capture_us,0.0125f);
g_last_track_capture_us=timing.capture_mono_us;

// :633-642 —— 发布 + 计数
g_uart.publish(tgt,timing);
g_last_pub_capture_us=timing.capture_mono_us;
g_last_pub_frame_id=timing.frame_id;
const double det_ms=(now()-td0)*1000.0;
det_total_ms+=det_ms; if(det_ms>g_det_max_ms)g_det_max_ms=det_ms;
const double proc_ms=timing.detect_duration_us()/1000.0;
g_proc_total_ms+=proc_ms; if(proc_ms>g_proc_max_ms)g_proc_max_ms=proc_ms;
const double lat_ms=timing.capture_latency_us()/1000.0;
g_lat_total_ms+=lat_ms; if(lat_ms>g_lat_max_ms)g_lat_max_ms=lat_ms;
++det_count;
```

5 秒日志新增（`:665-680`）：

```cpp
const gt::WriteStartAgeStats as=g_uart.take_write_start_age_stats();
const double cap_age_ms=g_last_pub_capture_us
    ? static_cast<double>(gt::monotonic_us()-g_last_pub_capture_us)/1000.0 : -1.0;
printf("TIMING last_frame_id=%u last_pub_frame_id=%u last_capture_age_ms=%.1f "
       "uart_write_start_age_last=%d uart_write_start_age_avg=%.2f uart_write_start_age_max=%d uart_write_start_age_n=%lu\n", ...);
const uint64_t win_delta=last_window_pts?(lastpts-last_window_pts):0;
printf("PTS_STAT last_raw=%llu window_delta_raw=%llu per_frame_raw=%.1f duplicate_pts=%lu\n", ...);
```

语义说明（复审时注意）：
- `last_capture_age_ms` 在**打印时刻**计算，而报告块紧跟在同一帧的发布之后，所以它约等于
  「该帧的处理延迟 + 打印开销」（实测 5.2~8.5ms）；`detect_stride=2` 的非检测帧会显示上一条发布的年龄（实测 12.8ms）。
- `DETECT avg_ms` **未改语义**（`td0` → 发布，含画框/日志/发布）；`proc_*`/`lat_*` 是**新增**的帧时间戳口径，
  所以 `avg≈proc` 恰好接近（画框/日志/发布很便宜），不是重复计算错误。

### 2.3 `src/uart_out.h`（94 → 130 行）/ `src/uart_out.cpp`（239 → 291 行）

```cpp
// uart_out.h:41-56
struct UartSnapshot {
  TargetOut target{};
  FrameTiming timing{};
  bool has_timing = false;   // false = 无可用时间戳（启动那一帧）
};
// 口径：采样点在 write_all() 之前 → 表示「取帧→开始写 UART」，不是「写完成」
struct WriteStartAgeStats {
  unsigned long samples = 0; int last_ms = -1; int max_ms = -1; double avg_ms = 0.0;
};

// uart_out.h:81-92
void publish(const TargetOut& out);                                // 保留原接口
void publish(const TargetOut& out, const FrameTiming& timing);      // 新增重载
WriteStartAgeStats take_write_start_age_stats();                    // 读取并清零本窗口
```

```cpp
// uart_out.cpp:251-258 —— 写之前重算「取帧→开始写 UART」年龄（只记录，不进 ASCII）
UartSnapshot snapshot;
{ std::lock_guard<std::mutex> g(mu_); if (has_latest_) { snapshot = latest_; has = true; } }
if (has) {
  const MonoUs write_start_us = monotonic_us();        // 命名带 write_start，避免被读成「写完成」
  add_write_start_age(
      frame_age_us(snapshot.has_timing, snapshot.timing.capture_mono_us, write_start_us));
  const int len = format_ascii(snapshot.target, frame, sizeof(frame));   // ← 未改动
  ...
}
```

线程/锁与容量约束：
- `mu_` 保护最新快照（生产者 `publish()` 一帧一次；消费者 50Hz 一次），**只保留最新值，无历史队列**；
- `write_age_mu_` 保护窗口统计（`write_age_samples_/write_age_sum_us_/write_age_last_ms_/write_age_max_ms_`），
  避免 sum 与 count 跨窗口错配；
- 无效样本（`has_timing=false`、`capture==0`、时间倒退）只更新 `last_ms=-1`，**不进入 avg/max/count**；
- `format_ascii()` 与 `write_all()` **一行未改** → `$GT,<det>,<yaw>,<pitch>,<vyaw>,<vpitch>,<dist>,<age>,<seq>,<state>*<CS>` 的字段数量、顺序、校验和算法完全不变；
- 未使用的既有成员 `published_seq_` 原样保留（未顺手清理，避免扩大 diff）。

### 2.4 `tests/test_phase1.cpp`（291 → 386 行）

新增第 [6] 段 `test_frame_timing()`（`:288`，`main()` 在 `:383` 调用），共 25 项：

| 组 | 覆盖 |
|---|---|
| 6.1 时钟 | `monotonic_us` 连续 1000 次不倒退、会前进、量级是微秒 |
| 6.2 dt | 12500us→0.0125s、25000us→0.025s（stride=2）、首帧无基准→fallback、时间倒退→fallback、两帧相同时刻→fallback |
| 6.3 年龄 | `capture=0`/无时间戳/`now<capture` → -1（**无符号下溢保护**）；15500us→15ms；999us→0ms |
| 6.4 耗时 | `detect_duration_us`=5500、`capture_latency_us`=7500、空对象→0、时间倒退→0、`valid()` |
| 6.5 编号 | `UartSnapshot` 同时保留 `seq`/`frame_id` 互不覆盖；启动帧 `has_timing=false` → 年龄 -1 |

同时新增 `#include "frame_timing.h"` / `"uart_out.h"`（后者用来验证 `UartSnapshot` 在主机侧也能编译）。

### 2.5 文档

- `docs/UART_PROTOCOL.md`（191 → 230 行）：
  - §2.1 `age` 行补「coast 期增长是设计行为，**不是**取帧→发送延迟」；
  - §3.2 增加 `age`（协议字段）与 `uart_write_start_age_*`（视觉侧日志）的区别说明；
  - 新增 **§六「视觉侧诊断日志（联调时对照用）」**（`:184`）：`capture_frames` / `DETECT` / `TIMING` / `PTS_STAT` / `UART` / `GREEN_TARGET` 各字段含义 + 三条纪律 + 本次实测数据；
  - 原 §六 改为 **§七「待电控确认的事项」**（`:215`），新增第 9 条「是否需要新增 `age_uart` 字段」。
- `README.md`（195 → 220 行）：功能列表加「时间戳与延迟可观测」；板端运行小节说明新日志行；
  新增 **§「时间戳与延迟观测（阶段1.1）」**（`:94`）表格 + 实测数据；单测项数 29 → 54；产物大小更新为
  467,440 字节（第二轮复核后为 467,448 字节，见 §11）。
- `docs/TIMESTAMP_LOGGING_IMPLEMENTATION_PLAN.md` **未修改**（作为规范保留原文），差异全部记在本文件 §1、§5。

---

## 3. 行为差异对照（改动前 → 改动后）

| 项 | 改动前 | 改动后 |
|---|---|---|
| 跟踪 dt | `now()`（**检测后**）差，混入抽样/检测/日志/调度抖动 | **采集时刻差**；`detect_stride>1` 时天然等于跨帧间隔 |
| 相机帧号 | 无 | `frame_id` 每取到一帧 +1（含 4s 预热与未检测帧） |
| 原始 PTS | 只用于 `pts_fps`/`duplicate_pts` | 同上 + `PTS_STAT` 原始值/窗口差值；仍**不参与计算** |
| 结果携带采集时间 | 无 | `UartSnapshot.timing`（POD 拷贝，不引用 `VIDEO_FRAME_INFO_S`） |
| UART `age` 字段 | 跟踪器测量年龄 | **不变**（方案 B） |
| 板端端到端年龄 | 不可观测 | 串口线程发送时重算 → `uart_write_start_age_last/avg/max/n`（5 秒窗口） |
| `GREEN_TARGET` 日志 | `misses==0` 时每帧（80Hz） | **≤10Hz**，尾部加 `frame=` |
| 5 秒日志 | 无帧号/延迟/PTS 明细 | `DETECT` 加 max/proc/lat；新增 `TIMING`、`PTS_STAT` |
| ASCII 协议 | — | **字段数量/顺序/校验算法完全不变** |
| 卡尔曼/检测算法 | — | **未改动**（`green_detect.*`、`target_track.*` 未触碰） |

---

## 4. 验证证据（真实输出，未造假）

### 4.1 主机单测

命令（计划 §9 原文）：

```bash
g++ -std=c++11 -O1 -Wall -Wextra -I src tests/test_phase1.cpp src/target_track.cpp src/green_detect.cpp -o /tmp/test_phase1
/tmp/test_phase1
```

| 时点 | 结果 | 警告 |
|---|---|---|
| 改动前 | `=== 结果：29 通过, 0 失败 ===` | 0 |
| 改动后 | `=== 结果：54 通过, 0 失败 ===` | 0 |

### 4.2 交叉编译（`env/verify_build.sh`，COMPILER/CVI_SDK_ROOT 同 README）

| 时点 | 产物 | md5 |
|---|---|---|
| 改动前 | 462,728 字节 | `6398a2f73b536680b55619e0048d5392` |
| 第一轮（时间戳改造） | 467,440 字节 | `7de0657fa61e7b23859d4de5aa654cac` |
| 第二轮（复核：日志改名 + 注释统一） | **467,448 字节** | `613f169ac8bf84f2692916efbb7ae061` |

- `configure rc=0` / `build rc=0`，构建日志 `warning:` 计数 = 0；
- 额外用交叉工具链 `-fsyntax-only -Wall -Wextra` 单独检查两个改动文件：
  - `src/camera.cpp`：改动前后**都是同样的 3 条既存警告**（1×`-Wsign-compare`、2×`-Wmisleading-indentation`，行号随插入代码从 542/565/566 平移到 561/586/587）→ **无新增警告**；
  - `src/uart_out.cpp`：前后均 0 条。

### 4.3 板端实测（LicheeRV Nano，`sh start.sh 20`，每场景 20 秒，无绿色目标）

> 下表是**第一轮**实测（2026-11-08 01:33~01:48）。当时字段名是 `uart_age_*`，第二轮改名为
> `uart_write_start_age_*`，**数值口径不变**；第二轮改名后的板端复测见本节末尾。

| 场景 | host_fps | duplicate_pts | DETECT | 备注 |
|---|---|---|---|---|
| A：`display=on, uart=false, stride=1` | 79.25~79.85 | 0 | avg 6.6~7.3ms，max 15.2~20.1ms，`proc≈avg`，`lat≈avg+0.02` | `last_frame_id==last_pub_frame_id`；`last_capture_age_ms` 5.2~8.5 |
| B：`display=off, uart=false, stride=1` | 79.95~80.83 | 0 | avg 5.1~6.4ms，max 9.1~16.9ms | 关编码后处理耗时下降 |
| C：`display=off, uart=true, stride=1` | 79.95~80.84 | 0 | avg 5.1~5.2ms | `UART sent rate 49.87~52.57Hz, errors=0`；`uart_age_last 6~13ms`、`avg 11.26~11.81ms`、`max 16~28ms`、`n≈250/5s` |
| D：`display=off, uart=false, stride=2` | 80.28~81.16 | 0 | calls **~200/5s**（stride=1 时 ~400） | `last_frame_id=785` vs `last_pub_frame_id=784` → 帧号每帧 +1、只在检测帧发布 |

Run A 关键原始日志（摘录）：

```text
capture_frames=1668 host_fps=79.2865 pts_fps=79.2895 duplicate_pts=0 timeouts=0 ...
DETECT avg_ms=6.617 max_ms=15.227 proc_avg_ms=6.616 proc_max_ms=15.224 lat_avg_ms=6.622 lat_max_ms=15.230 calls=398 stride=1 map_miss=16 sample_ms=4.725 track_ms=1.888 ns=1 cand_max=8
TIMING last_frame_id=1965 last_pub_frame_id=1965 last_capture_age_ms=8.0 uart_age_last=-1 uart_age_avg=0.00 uart_age_max=-1 uart_age_n=0
PTS_STAT last_raw=35158315216 window_delta_raw=5012516 per_frame_raw=12594.3 duplicate_pts=0
```

Run C 关键原始日志（摘录，UART 打开）：

```text
UART: /dev/ttyS1 @ 115200 8N1, rate=50Hz, format=ascii
UART sent=1264 rate=50.07Hz errors=0
TIMING last_frame_id=1982 last_pub_frame_id=1982 last_capture_age_ms=5.3 uart_age_last=8 uart_age_avg=11.53 uart_age_max=28 uart_age_n=251
```

第二轮（改名后）复测，`display=off, uart=true, stride=1`，新二进制 md5 `613f169a…`：

```text
UART: /dev/ttyS1 @ 115200 8N1, rate=50Hz, format=ascii
UART sent=1264 rate=49.99Hz errors=0
DETECT avg_ms=5.141 max_ms=13.165 proc_avg_ms=5.137 proc_max_ms=13.162 lat_avg_ms=5.144 lat_max_ms=13.168 calls=399 stride=1 map_miss=16 sample_ms=4.722 track_ms=0.412 ns=1 cand_max=8
TIMING last_frame_id=1983 last_pub_frame_id=1983 last_capture_age_ms=5.3 uart_write_start_age_last=8 uart_write_start_age_avg=11.92 uart_write_start_age_max=20 uart_write_start_age_n=250
PTS_STAT last_raw=36504491147 window_delta_raw=5000002 per_frame_raw=12531.3 duplicate_pts=0
```

→ 新字段名生效，数值与第一轮一致（`avg` 11.43~11.92ms、`max` 17~20ms、`n≈250/5s`）；
本轮 `capture_frames` 窗口 80.14~80.84fps、`duplicate_pts=0`；supervisor 输出
`TEST_COMPLETED` 与 `FINAL_CLOCK=0x00200001`（相机 PLL 时钟已复位）。

**PTS 单位（计划 §9 的未决项）已在板上测定**：

```
per_frame_raw ≈ 12421 ~ 12722（四次运行的 4 个窗口）
window_delta_raw ≈ 5,000,000 / 5s
duplicate_pts = 0（4 个场景全程）
```

→ **`u64PTS` 单位是微秒**，且单调递增。代码仍只记录、不参与任何计算。

**一致性交叉验证**：`uart_write_start_age_avg ≈ 11.5ms` 与理论值吻合
（50Hz 发送平均等半个帧周期 6.25ms + 视觉处理 5.1ms ≈ 11.35ms），说明端到端年龄的计算口径正确；
`lat_avg - proc_avg ≈ 0.03ms` 说明「取帧→开始检测」确实只有几十微秒。

### 4.4 复现命令

```bash
# 主机单测（WSL）
wsl -e bash -c "cd /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget && \
  g++ -std=c++11 -O1 -Wall -Wextra -I src tests/test_phase1.cpp \
      src/target_track.cpp src/green_detect.cpp -o /tmp/t && /tmp/t"

# 交叉编译（注意：不要用 build.sh，见 §6.1）
wsl -e bash /mnt/e/study/LicheeRV/env/verify_build.sh

# 板端：部署 + 跑一轮 + 看新日志
scp build/bin/camera root@10.133.124.1:/code/fly_LicheeRV/camera
ssh root@10.133.124.1 'cd /code/fly_LicheeRV && sh start.sh 20'
ssh root@10.133.124.1 "grep -E '^(TIMING|PTS_STAT|DETECT|UART)' /tmp/fly_licheerv_capture.log"
```

---

## 5. 未做 / 未验证（明确列出，勿误认为已完成）

1. **连续绿色目标场景未验证**：板子前面没有绿色灯，`GREEN_STAT hits=0`。
   因此「`GREEN_TARGET` 限频 10Hz 实际生效」只有代码层面成立，**没有实测日志证明**。
2. **目标丢失 / 恢复（coast → lost → 重新捕获）未验证**：同样需要真实目标。
3. **UART 字节内容未抓包**：没有第二路串口抓取 `/dev/ttyS1` 的输出，只能证明「打开成功、50Hz、errors=0、
   `age` 字段来源未变（`format_ascii` 一行未改）」。校验和与字段顺序靠代码审查而非抓包。
4. **P99 未统计**：5 秒窗口只输出 avg/max，没有 P99；需要时可在 `DETECT`/`TIMING` 行加直方图。
5. **计划 §7 第二阶段（有界队列日志线程、async_log）未实现**，`log_drop_count` 因此不存在。
6. **未改** `build.sh`、`CMakeLists.txt`、`start.sh`、`config.yaml`（`detect_stride` 等测试用的临时改动已在板上恢复原状）。
7. 计划 §10 验收项「UART 发送线程重新计算 age」只完成一半：算出来并用在日志，**没有**写进协议字段（方案 B）。

---

## 6. 已知问题 / 环境坑（与本次改动无关，但会误导后来者）

### 6.1 `build.sh` 的 SDK 路径在本机失效（**未修，建议单独处理**）

- `build.sh:23` 默认 `CVI_SDK_ROOT=/mnt/e/study/LicheeRV/env/sipeed-sdk/middleware/v2`，
  但 `build.sh:25` 校验的是 `$CVI_SDK_ROOT/middleware/include`，该路径不存在 →
  直接 `wsl -e bash build.sh` 会打印「缺少平台 SDK」并退出。
- 本机可用的是 `E:\study\LicheeRV\env\sipeed-sdk-build`（`include/`+`kernel_include/`+`lib/`+`lib_dyn/`），
  与 `CMakeLists.txt` 的默认 `SDK_ROOT` 一致。因此本次交叉编译使用 `env/verify_build.sh`（HANDOFF 文档里的正确入口）。
- 连带影响：`README.md` 第 45 行的编译命令也指向 `build.sh`，与现状不符。
- **建议**：把 `build.sh` 的默认 `CVI_SDK_ROOT` 改成 `.../env/sipeed-sdk-build`，并把校验改成 `[ -d "$CVI_SDK_ROOT/include" ]`。

### 6.2 WSL 的 `/tmp` 在多次 `wsl.exe` 调用之间会被清空

本次实施中 WSL 实例被系统重启（`uptime 0 min`），放在 `/tmp` 的基线源码副本被清掉，
导致「改动前后文件 diff」只能用同参数重编译的警告日志对比。**后续把重要中间产物放 Windows 侧**。

### 6.3 PowerShell 5.1 的 `Get-Content` 对本仓库部分文件计数不准

`Get-Content README.md` 报 149 行，实际 220 行（`[IO.File]::ReadAllText` + `\n` 计数、以及编辑器行号都一致为 220）。
统计行数请用 `.NET ReadAllText` 或直接 `wc -l`，不要用 PS 5.1 的 `Get-Content`。

---

## 7. 请 Codex 重点复审的点

1. **方案 B 是否可接受**：`age` 语义不变、端到端年龄只进日志。若产品侧决定改成 A/C，改动点见 §1.1。
2. **`capture_dt_s()` 与原计划片段是否严格等价**（`prev==0` / `<=` 都走 fallback；`camera.cpp:574`）。
3. **dt 与 `TargetTracker::update(now_ms)` 的口径不完全一致**（dt 用采集时刻，`age_ms` 仍用处理完成时刻
   `t_now*1000.0`）——这是**刻意保留**的（计划未要求改，改了会让 UART `age` 数值整体偏移约一个检测耗时）。
   请确认是否认可，或建议改成统一口径。
4. **`last_capture_age_ms` 的口径**：它在报告时刻计算，恰好紧跟在同帧发布之后，所以约等于「处理延迟」而不是
   「发布后经过的时间」。若更想要「结果多久没更新」，应改成在上一次发布处记时间戳再相减。
5. **`age_mu_` 与 `mu_` 两把锁的取值顺序**：当前 `take_write_start_age_stats()` 在 5 秒日志里调用，与发送线程只竞争
   统计锁，不与快照锁嵌套，无死锁风险；但将来若合并统计到快照结构里，需重新评估。
6. **`detect_stride>1` 时 dt 的实际值没有直接日志**（只通过 `frame_id` 与 `pub_frame_id` 差值间接验证）。
   若需要，可在 `DETECT` 行加 `dt_avg_ms`。
7. **第二阶段是否现在就做**：计划要求「只有确实需要逐帧文件日志时才加线程」。当前 `GREEN_TARGET` 已限频
   10Hz、其余 5 秒一次，板端 4 场景没有任何日志压力证据 → 建议**保持不加**。

---

## 8. 回退与部署

**板端**（当前状态见 §9）：
```sh
ssh root@10.133.124.1 'cd /code/fly_LicheeRV && cp -a camera.bak-20261108-before-timing camera'
```

**源码**：本次涉及 7 个文件，逐个 `git checkout --`/手动还原即可，**不要** `git reset --hard` / `git clean -fd`
（工作区里还有大量他人/前序未提交改动：`CMakeLists.txt`、`start.sh`、`config*.yaml`、`src/green_detect.*`、
`src/target_track.*`、`docs/*`、`tests/*` 等）。

| 文件 | 能否单独回退 |
|---|---|
| `src/frame_timing.h` | 新增文件，删除即回退（但 `camera.cpp`/`uart_out.*`/测试同时要还原） |
| `src/camera.cpp` | 7 处独立改动（§2.2 表），可逐处还原 |
| `src/uart_out.h` / `.cpp` | 快照结构 + 统计，可还原（保留 `publish(out)` 的旧行为） |
| `tests/test_phase1.cpp` | 删除第 [6] 段与两个 include 即回到 29 项 |
| `docs/UART_PROTOCOL.md` / `README.md` | 纯文档 |

---

## 9. 当前环境状态（交接用）

**板端 10.133.124.1**
- 运行中进程：无；`.camera.lock`：无；时钟寄存器 `0x030028c4 = 0x00200001`（原值）；温度 58.8°C。
- `/code/fly_LicheeRV/camera`：**已部署第二轮构建**（md5 `613f169a…`，467,448 字节）。
- 备份：`camera.bak-20261108-r1`（第一轮，md5 `7de0657f…`）、
  `camera.bak-20261108-before-timing`（改动前，md5 `6398a2f7…`）。
- `config.yaml` 已恢复原样（md5 `07c163f4…`），另有同内容备份 `config.yaml.bak-20261108-timing`
  与 `config.yaml.bak-20261108-r2`。
- 日志：`/tmp/gt_runA.log`、`gt_runB.log`、`gt_runC.log`、`gt_runD.log`（第一轮）、`gt_r2.log`（第二轮）保留（tmpfs，重启即失），
  其余临时脚本已删。

**WSL**
- 本次用过的 `/tmp/gt_timing/`、`/tmp/gt_final/`、`/tmp/gt_r2/`、`/tmp/vb1.log`、`/tmp/vb2.log` **已删除**；
  `ls /tmp` 现只剩系统目录；家目录无新增文件。

**Windows**
- `%TEMP%\gt_wsl\`（本次所有 WSL/板端脚本）**已删除**。
- 仓库内 `build/` 为本次重新生成（gitignore 内），无其它临时文件。

**WSL**
- 本次用过的 `/tmp/gt_timing/`、`/tmp/gt_final/`、`/tmp/gt_r2/`、`/tmp/vb1.log`、`/tmp/vb2.log` **已删除**；
  `ls /tmp` 现只剩系统目录；家目录无新增文件。

**Windows**
- `%TEMP%\gt_wsl\`（本次所有 WSL/板端脚本）**已删除**。
- 仓库内 `build/` 为本次重新生成（gitignore 内），无其它临时文件。

---

## 10. 结论

- 计划第一阶段中「可观测性」部分已完整落地：**帧号、取帧时刻、原始 PTS、检测耗时、跟踪帧间隔、
  UART「取帧→开始写」年龄、重复 PTS、日志限频**都可从 5 秒日志读出，且不改协议、不动算法、
  不引入线程/队列/进程。
- 唯一实质偏差是 `age` 字段按用户决策保持原语义（方案 B）；若产品需要协议级端到端年龄，改动点很小（§1.1、§11.3）。
- 剩余风险集中在**未验证项**（连续目标、coast 恢复、UART 抓包），而不是已知缺陷。
- 第二轮已按 Codex 复核意见完成「日志改名 + 注释统一」（§11），代码行为未变。

---

## 11. 第二轮复核修订（Codex 评审，2026-11-08）

Codex 对第一版实现提出 2 条技术建议 + 1 条待确认项，处理如下。

### 11.1 采纳：UART 年龄日志改名 `uart_write_start_age`

**意见**：年龄在 `write_all()` **之前**采样，准确含义是「取帧 → 开始写 UART」，
建议二选一：改名，或把采样移到 `write_all()` 返回之后改叫 `uart_write_complete_age`。

**决策**：采用**改名**，采样位置不动。理由：

1. 零行为改动（只改标识符与日志字段名），不碰发送时序，不需要重新评估串口线程行为；
2. 与「将来若改用方案 C」的计算时刻一致 —— C 的协议 `age` 只能在 `format_ascii()` 之前算，
   那一刻也只能是 write-start；日志与协议字段同口径，便于互相对照；
3. 两个口径的差异有界：115200 下 60 字节通常在内核缓冲里立即返回（<0.1ms），
   只有缓冲满时 `write_all` 才重试（上限约 2ms），因此「开始写」与「写完成」最多差约 2ms。

**改动**：`WriteStartAgeStats`、`take_write_start_age_stats()`、`add_write_start_age()`、
成员 `write_age_*`、日志字段 `uart_write_start_age_last/avg/max/n`；并在 `uart_out.h`、`uart_out.cpp`、
`docs/UART_PROTOCOL.md` 注明「write_start 口径、不含 write 阻塞」，同时写明「若需要写完成口径，
应另立 `uart_write_complete_age_*` 并把采样移到 `write_all()` 之后，不要复用现有字段」。

### 11.2 采纳：统一 `u64PTS` 单位表述

报告/README 已写「实测为微秒」，但代码注释仍写「单位未实测确认」。已统一：

| 位置 | 改为 |
|---|---|
| `src/frame_timing.h:7`（文件头约定） | 板端实测（2026-11-08，1280x720/80fps）单位是微秒、每帧差≈12500，见报告 §4.3；换传感器/驱动需重新确认，代码不依赖该单位 |
| `src/frame_timing.h:31`（字段注释） | VI 原始 PTS（板端实测为微秒；只记录，不参与计算） |
| `src/camera.cpp:39-41`（静态量注释） | u64PTS 板端实测单位是微秒…仍只记录、不参与任何计算 |
| `src/camera.cpp:674-675`（`PTS_STAT` 注释） | 板端实测单位是微秒…但代码不做换算 |
| `docs/UART_PROTOCOL.md` §六 `PTS_STAT` 行 | 已实测为微秒…代码仍只观测、不做任何换算 |
| `README.md`「时间戳与延迟观测」表格 | 只记录不换算（板端实测为微秒，80fps 时每帧差≈12500） |

### 11.3 待确认项（本次未改）：`age` 仍为方案 B

Codex 指出「当前实现是 B，不是此前推荐的 C」，需要重新确认。**本次没有改**，原因：

- 实施前的方案选择中你明确选了 B（协议语义不变、端到端年龄只进日志），本次没有新的授权；
- 工作区仍没有「电控侧是否已按旧协议实现 `age` 阈值」的证据，改协议语义的收益/风险比没有变化。

若决定改用 C，改动很小（**两处**）：

1. `src/uart_out.cpp:208` `format_ascii()` 里，`const float age = (out.age_ms < 0.0f) ? -1.0f : out.age_ms;`
   改为 `std::max(out.age_ms, 该快照的 write-start 年龄)`；这需要把年龄传进 `format_ascii()`
   （当前在 `thread_main()` 里算完就只用于统计），即给 `format_ascii` 增加一个 `int age_ms` 参数；
2. `docs/UART_PROTOCOL.md` §2.1/§3.2/§六 同步改字段说明（coast 期仍会增长；正常检测时
   ≈ 端到端年龄 11~12ms）。§七 第 9 条已把「是否新增/改 `age` 语义」列为待电控确认项。

### 11.4 第二轮验证

| 项 | 结果 |
|---|---|
| 主机单测 | 54/54 通过，0 警告 |
| 交叉编译 | `configure rc=0` / `build rc=0`，**467,448 字节**，md5 `613f169ac8bf84f2692916efbb7ae061`，构建日志 0 警告 |
| 交叉 `-Wall -Wextra` | `camera.cpp` 仍是那 3 条既存警告（无新增）；`uart_out.cpp` 0 条 |
| 板端复测（`display=off, uart=true`，20s） | 80.14~80.84fps、`duplicate_pts=0`、UART 49.87~49.99Hz / errors=0、`uart_write_start_age avg 11.43~11.92ms`、`TEST_COMPLETED` 且时钟复位 `0x00200001` |
