// 帧时间戳：相机帧号 + 采集时刻 + 原始 VI PTS
//
// 设计要点：
//   1. 统一用 CLOCK_MONOTONIC 和 uint64_t 微秒，避免 double 秒的精度/单位混淆；
//   2. capture_mono_us 表示「程序拿到这一帧」的时刻，**不是曝光时刻**；
//      曝光→出图的延迟需要另外标定，不能从这里的差值推算；
//   3. vi_pts_raw 只保存和统计，**不参与任何计算**。板端实测（2026-11-08，1280x720/80fps）
//      单位是微秒、每帧差≈12500，见 docs/TIMESTAMP_LOGGING_IMPLEMENTATION_REPORT.md §4.3；
//      换传感器或换驱动后需重新确认，代码不依赖该单位；
//   4. 帧号 frame_id 与 TargetOut::seq（结果序号）是两套独立编号，不得混用：
//      frame_id 每取到一帧 +1，seq 只在跟踪器状态更新时 +1。
#ifndef GREEN_TARGET_FRAME_TIMING_H_
#define GREEN_TARGET_FRAME_TIMING_H_

#include <cstdint>
#include <ctime>

namespace gt {

using MonoUs = uint64_t;

// 单调时钟微秒。不受系统校时影响，只用于求差（不要直接当日期时间用）。
inline MonoUs monotonic_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<MonoUs>(ts.tv_sec) * 1000000ULL +
         static_cast<MonoUs>(ts.tv_nsec) / 1000ULL;
}

struct FrameTiming {
  uint32_t frame_id = 0;        // 相机帧号：每成功取到一帧 +1
  uint64_t vi_pts_raw = 0;      // VI 原始 PTS（板端实测为微秒；只记录，不参与计算）
  MonoUs capture_mono_us = 0;   // 取帧成功时刻
  MonoUs detect_start_us = 0;   // 开始抽样/检测
  MonoUs detect_end_us = 0;     // 检测+跟踪+角度换算完成

  bool valid() const { return frame_id != 0 && capture_mono_us != 0; }

  // 抽样→结果完成 的耗时（微秒）；未记录或时间倒退时返回 0
  uint32_t detect_duration_us() const { return diff_us(detect_start_us, detect_end_us); }

  // 取帧→结果完成 的板端延迟（微秒）；未记录时返回 0
  uint32_t capture_latency_us() const { return diff_us(capture_mono_us, detect_end_us); }

 private:
  static uint32_t diff_us(MonoUs from, MonoUs to) {
    if (from == 0 || to < from) {
      return 0;
    }
    const MonoUs d = to - from;
    return d > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(d);
  }
};

// 两帧采集时刻差（秒）。prev 为 0 或时间倒退时返回 fallback；
// 结果再由 KalmanCfg 的 dt_min/dt_max 裁剪（见 TargetTracker::update）。
// 用两个 uint64 相减再转浮点，避免 double 表示 1e12 级微秒时的精度损失。
inline float capture_dt_s(MonoUs capture_mono_us, MonoUs prev_capture_mono_us, float fallback) {
  if (prev_capture_mono_us == 0 || capture_mono_us <= prev_capture_mono_us) {
    return fallback;
  }
  return static_cast<float>(capture_mono_us - prev_capture_mono_us) * 1e-6f;
}

// 取帧→「现在」（通常是 UART 发送时刻）的板端年龄（微秒）。
// 没有时间戳、或时间倒退（理论上不会发生）时返回 -1，避免无符号下溢成天文数字。
inline int64_t frame_age_us(bool has_timing, MonoUs capture_mono_us, MonoUs now_mono_us) {
  if (!has_timing || capture_mono_us == 0 || now_mono_us < capture_mono_us) {
    return -1;
  }
  const MonoUs d = now_mono_us - capture_mono_us;
  return d > 0x7FFFFFFFFFFFFFFFULL ? static_cast<int64_t>(0x7FFFFFFFFFFFFFFFLL)
                                   : static_cast<int64_t>(d);
}

// 同上，毫秒（向下取整）；无效时 -1。
inline int frame_age_ms(bool has_timing, MonoUs capture_mono_us, MonoUs now_mono_us) {
  const int64_t us = frame_age_us(has_timing, capture_mono_us, now_mono_us);
  if (us < 0) {
    return -1;
  }
  const int64_t ms = us / 1000;
  return ms > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(ms);
}

}  // namespace gt

#endif  // GREEN_TARGET_FRAME_TIMING_H_
