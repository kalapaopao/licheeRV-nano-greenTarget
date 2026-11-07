# LicheeRV 飞镖视觉时间戳与异步日志改造执行方案

> 给 DeepSeek Harness（DSH）执行。项目：E:/study/LicheeRV/licheeRV-nano-greenTarget。  
> 本次只改时间戳、跟踪 dt、UART 数据年龄和日志路径；不引入模型识别、IMU、世界坐标或独立日志进程。

## 1. 目标和原则

目标链路：

~~~text
VI 取帧 -> 建立帧元数据 -> 抽样/检测/跟踪
-> 结果携带帧号和采集时间 -> UART 发送前重新计算年龄
-> 日志限频；确有需要时再增加有界日志线程
~~~

必须能观测：

- 相机帧号；
- 取帧时刻；
- 原始 VI PTS；
- 检测耗时；
- 跟踪使用的帧间隔；
- UART 发送时结果年龄；
- 重复 PTS、超时、日志丢弃。

约束：

- 不修改 green_detect 算法；
- 不改变 VI/VENC/HTTP 主架构；
- 不修改现有 UART ASCII 字段数量和顺序；
- 不把 VIDEO_FRAME_INFO_S 裸引用传给异步线程；
- 不使用无界队列；
- 当前不增加日志进程。

## 2. 当前问题

当前 src/camera.cpp 已有 CLOCK_MONOTONIC 的 now()、u64PTS 统计、TargetTracker::update、UartOut 发送线程和 5 秒统计日志，但存在以下问题：

1. u64PTS 的单位没有实测确认，不能直接假设是微秒。
2. 跟踪 dt 使用检测区域的 now() 差值，混入抽样、检测、日志和调度抖动；应使用采集时刻差。
3. g_uart.publish(tgt) 没有传递采集帧号和采集时间。
4. TargetOut.age_ms 是跟踪器内部年龄，不是完整的“取帧到 UART 发送”年龄；UART 线程没有重新计算它。
5. GREEN_TARGET 在 tgt.misses == 0 时可能每帧 printf，可能影响 80 FPS 主循环。
6. TargetOut.seq 是结果序号，不是相机帧号，不能混用。

## 3. 时间定义

统一使用 CLOCK_MONOTONIC 和 uint64_t 微秒：

- vi_pts_raw：VI 原始 PTS，只保存和统计，单位确认前不参与计算；
- capture_mono_us：CVI_VI_GetChnFrame 成功返回后的单调时刻；
- detect_start_us/detect_end_us：检测处理时间；
- send_us：UART 线程准备发送时刻；
- age_ms：send_us - capture_mono_us，表示板端结果年龄。

capture_mono_us 不能叫曝光时间，它表示“程序拿到帧的时间”。

## 4. 建议新增 src/frame_timing.h

~~~cpp
#ifndef GREEN_TARGET_FRAME_TIMING_H_
#define GREEN_TARGET_FRAME_TIMING_H_

#include <cstdint>
#include <ctime>

namespace gt {

using MonoUs = uint64_t;

inline MonoUs monotonic_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<MonoUs>(ts.tv_sec) * 1000000ULL
       + static_cast<MonoUs>(ts.tv_nsec) / 1000ULL;
}

struct FrameTiming {
  uint32_t frame_id = 0;
  uint64_t vi_pts_raw = 0;
  MonoUs capture_mono_us = 0;
  MonoUs detect_start_us = 0;
  MonoUs detect_end_us = 0;

  bool valid() const {
    return frame_id != 0 && capture_mono_us != 0;
  }

  uint32_t detect_duration_us() const {
    if (detect_end_us < detect_start_us) return 0;
    const MonoUs d = detect_end_us - detect_start_us;
    return d > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(d);
  }
};

}  // namespace gt
#endif
~~~

如果不新增文件，也可以放在已有公共头文件，但不要放进 green_detect.h。

## 5. camera.cpp 修改

增加：

~~~cpp
#include "frame_timing.h"

static uint32_t capture_frame_id = 0;
static uint64_t last_track_capture_us = 0;
~~~

在 CVI_VI_GetChnFrame() 成功后立即执行：

~~~cpp
gt::FrameTiming timing{};
timing.frame_id = ++capture_frame_id;
timing.vi_pts_raw = frame.stVFrame.u64PTS;
timing.capture_mono_us = gt::monotonic_us();
~~~

进入检测前记录：

~~~cpp
timing.detect_start_us = gt::monotonic_us();
~~~

检测、跟踪、结果准备完成后记录：

~~~cpp
timing.detect_end_us = gt::monotonic_us();
~~~

把跟踪 dt 改成：

~~~cpp
float dt = 0.0125f;
if (last_track_capture_us != 0 &&
    timing.capture_mono_us > last_track_capture_us) {
  dt = static_cast<float>(
      timing.capture_mono_us - last_track_capture_us) * 1e-6f;
}
last_track_capture_us = timing.capture_mono_us;
~~~

继续保留 KalmanCfg 的 dt_min/dt_max 裁剪。detect_stride > 1 时，只在真正调用 Tracker::update 的帧上更新 last_track_capture_us。

不要把 timing.frame_id 写入 TargetOut::seq；seq 保持原语义。

## 6. UART 修改

在 src/uart_out.h 增加：

~~~cpp
#include "frame_timing.h"

struct UartSnapshot {
  TargetOut target{};
  FrameTiming timing{};
  bool has_timing = false;
};
~~~

保留原 publish，并增加重载：

~~~cpp
void publish(const TargetOut& out);
void publish(const TargetOut& out, const FrameTiming& timing);
~~~

camera.cpp 使用：

~~~cpp
g_uart.publish(tgt, timing);
~~~

UartOut 内部只保留最新 UartSnapshot，不建立历史结果队列。

在 UART 发送线程取得快照后重新计算：

~~~cpp
const uint64_t send_us = gt::monotonic_us();
int age_ms = -1;

if (snapshot.has_timing &&
    snapshot.timing.capture_mono_us != 0 &&
    send_us >= snapshot.timing.capture_mono_us) {
  const uint64_t age_us =
      send_us - snapshot.timing.capture_mono_us;
  age_ms = age_us > 0x7FFFFFFFULL
         ? 0x7FFFFFFF
         : static_cast<int>(age_us / 1000ULL);
}
~~~

ASCII 格式保持：

~~~text
$GT,<det>,<yaw>,<pitch>,<vyaw>,<vpitch>,<dist>,<age>,<seq>,<state>*<CS>
~~~

第一阶段不要增加字段，避免破坏电控解析。修改 docs/UART_PROTOCOL.md，明确 age 是“对应图像帧被视觉程序取到后，到当前 UART 发送时刻的板端年龄”，无有效时间时为 -1。

## 7. 日志方案

### 第一阶段：只限频

不新增线程，先把 GREEN_TARGET 限制为 5~10 Hz：

~~~cpp
static uint64_t last_target_log_us = 0;
const uint64_t log_now = gt::monotonic_us();

if (log_now - last_target_log_us >= 100000) {
  last_target_log_us = log_now;
  printf("GREEN_TARGET ...");
}
~~~

保持 GREEN_STAT、FPS、PTS、检测耗时约每 5 秒一次。每帧只更新计数器，不同步打印。

### 第二阶段：只有确实需要逐帧文件日志时才加线程

新增 async_log.h/.cpp 时，使用固定容量 256/512 的有界队列：

~~~cpp
struct LogEvent {
  uint32_t frame_id;
  uint64_t capture_mono_us;
  uint64_t detect_end_us;
  float yaw_deg;
  float pitch_deg;
  float v_yaw_dps;
  float v_pitch_dps;
  uint8_t state;
  uint8_t detected;
  uint16_t candidates;
};
~~~

要求：

- try_push 不等待；
- 队列满时丢弃事件并递增 log_drop_count；
- 日志线程批量写入；
- 不每行 fsync；
- 退出时 flush；
- 日志线程不能阻塞取帧、检测、跟踪或 UART 最新结果。

当前不要增加独立日志进程。进程 IPC 只在未来需要网络传输、复杂压缩或故障隔离时考虑。

## 8. 文件范围

第一阶段新增：

~~~text
src/frame_timing.h
~~~

第一阶段修改：

~~~text
src/camera.cpp
src/uart_out.h
src/uart_out.cpp
docs/UART_PROTOCOL.md
README.md（如需补充时间字段说明）
~~~

第二阶段可选：

~~~text
src/async_log.h
src/async_log.cpp
CMakeLists.txt
tests/test_frame_timing.cpp
~~~

不要修改：

~~~text
src/green_detect.cpp
src/green_detect.h
src/target_track.cpp
~~~

除非编译兼容性证明必须调整。

## 9. 测试计划

### 主机回归

~~~bash
g++ -std=c++11 -O1 -Wall -Wextra -I src \
  tests/test_phase1.cpp src/target_track.cpp src/green_detect.cpp \
  -o /tmp/test_phase1
/tmp/test_phase1
~~~

记录真实输出，不得伪造通过结果。

### 时间测试

至少验证：

1. monotonic_us 连续调用不倒退；
2. 采集时间差计算正确；
3. capture_mono_us=0 时 age=-1；
4. now < capture 时不发生无符号下溢；
5. detect_duration_us 正确；
6. frame_id 与 TargetOut::seq 独立。

### PTS 验证

板端至少运行 5 秒，记录 frame_id、u64PTS、delta_pts，确认 PTS 是否单调、是否重复、差值是否稳定。80 FPS 理论间隔约 12500，但未实测前不能假设单位。

### 板端组合测试

使用现有 build.sh 后测试：

1. display_enable=false；
2. display_enable=true；
3. UART 关闭；
4. UART 开启；
5. 连续目标；
6. detect_stride=2；
7. 目标丢失/恢复。

对比 capture_fps、pts_fps、detect 时间、UART rate、age 和日志输出频率。

每 5 秒输出：

~~~text
capture_fps
pts_fps
duplicate_pts
vi_timeouts
detect_calls
detect_avg_ms
detect_max_ms
sample_avg_ms
track_avg_ms
last_frame_id
last_capture_age_ms
uart_sent_rate
uart_send_errors
log_drop_count
~~~

## 10. 验收标准

必须满足：

- 原有主机测试不回归；
- frame_id 单调递增；
- capture_mono_us 单调递增；
- dt 使用采集时间差；
- UART 发送线程重新计算 age；
- UART 字段数量和顺序不变；
- 连续目标时 GREEN_TARGET 不再每帧同步打印；
- 不使用无界队列；
- 编译无新增警告。

需要记录但不阻塞第一阶段：

- PTS 真实单位；
- 取帧到检测完成的最大/P99 延迟；
- 取帧到 UART 发送的最大/P99 年龄；
- UART 调度抖动；
- 日志丢弃数量。

## 11. 风险和回退

风险：

- 不要把 capture_mono_us 称为曝光时间；
- 不要未经实测把 u64PTS 当微秒；
- 时间相减前检查 now >= capture；
- 不要改变 UART 字段顺序；
- 不要把 VIDEO_FRAME_INFO_S 裸引用交给异步线程；
- 不要用无界队列；
- 不要让日志阻塞识别。

查看差异：

~~~powershell
git -C E:\study\LicheeRV\licheeRV-nano-greenTarget status --short
git -C E:\study\LicheeRV\licheeRV-nano-greenTarget diff --stat
git -C E:\study\LicheeRV\licheeRV-nano-greenTarget diff
~~~

回退只恢复本次文件：

~~~text
src/frame_timing.h
src/camera.cpp
src/uart_out.h
src/uart_out.cpp
docs/UART_PROTOCOL.md
README.md
~~~

不要使用 git reset --hard 或 git clean -fd，因为当前工作区已有其他未提交修改。

## 12. 执行顺序

1. 检查当前 git status 和相关源文件；
2. 新增 frame_timing.h；
3. 取帧成功后建立 FrameTiming；
4. 用 capture_mono_us 改写跟踪 dt；
5. 给 UART 快照增加 FrameTiming；
6. UART 发送时重新计算 age；
7. 保持旧 ASCII 字段顺序；
8. GREEN_TARGET 限频到 5~10 Hz；
9. 执行主机测试；
10. 交叉编译板端程序；
11. 板端验证 PTS、FPS、检测耗时、age 和日志影响；
12. 只有限频仍不足时，再增加有界日志线程；
13. 当前不要增加日志进程。

## 13. 结论

最小、推荐方案：

~~~text
时间戳：取帧处创建，沿结果传递
跟踪 dt：使用采集时间差
UART age：发送时重新计算
结果队列：只保留最新值
日志：先限频，必要时再加有界日志线程
进程：当前不需要
~~~

时间戳不会自动保证实时性，但能让采集时间、检测时间、发送时间、数据年龄和丢帧情况可测量；先完成这层小改造，再根据真实 P99 延迟决定是否扩大线程化范围。
