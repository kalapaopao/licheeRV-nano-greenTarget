// 串口输出：把跟踪结果按固定节奏发给电控
//
// 设计要点：
//   1. 独立线程发送，主线程 publish() 只做一次结构体拷贝（微秒级）
//   2. 固定频率发送（默认 50Hz），**即使没有目标也发**，让电控知道视觉在线
//   3. 默认 ASCII 格式（串口助手可直接看），可选紧凑二进制
//   4. 打开失败不阻塞主流程：记录错误后静默降级，检测/网页不受影响
//   5. 快照同时携带该帧的采集时间戳（frame_timing.h）：串口线程在真正发送时
//      重新计算「取帧→发送」板端年龄，供诊断日志使用。ASCII 的 age 字段
//      仍是「距最近一次真实检测的毫秒数」，语义不变（见 docs/UART_PROTOCOL.md）。
//
// ASCII 帧格式（NMEA 风格，带 XOR 校验）：
//   $GT,<det>,<yaw>,<pitch>,<vyaw>,<vpitch>,<dist>,<age>,<seq>,<state>*<CS>\r\n
//   例：$GT,1,12.34,-5.67,0.12,-0.03,3.456,15,1234,2*5A
//
//   字段说明：
//     det    1=有目标 0=无
//     yaw    水平角（度，正=右）
//     pitch  俯仰角（度，正=上）
//     vyaw   水平角速度（度/秒）
//     vpitch 俯仰角速度（度/秒）
//     dist   距离（米，无效时 0.000）
//     age    数据年龄（毫秒，-1 表示从未测到）
//     seq    序号
//     state  0=lost 1=coasting 2=confirmed
//   CS     '$' 与 '*' 之间所有字符的 XOR（两位大写十六进制）
#ifndef GREEN_TARGET_UART_OUT_H_
#define GREEN_TARGET_UART_OUT_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "frame_timing.h"
#include "target_track.h"

namespace gt {

// 一帧结果的快照：目标结果 + 该帧的时间戳（由 camera.cpp 在取帧时创建）
struct UartSnapshot {
  TargetOut target{};
  FrameTiming timing{};
  bool has_timing = false;   // false = 没有可用时间戳（例如启动时那一帧）
};

// 「取帧 → 开始写 UART」的板端年龄统计（诊断用，不进入 ASCII 协议字段）。
// 口径：采样点在 write_all() **之前**，所以是「写开始」而不是「写完成」。
// 115200 下 60 字节通常在内核缓冲里立即返回（<0.1ms），缓冲满时 write_all 最多重试约 2ms；
// 若将来需要「写完成」口径，应改名并移动到 write_all() 返回之后，不要复用本字段。
struct WriteStartAgeStats {
  unsigned long samples = 0;   // 本统计窗口内的有效样本数
  int last_ms = -1;            // 最近一次发送时的年龄；-1 = 无有效时间戳
  int max_ms = -1;             // 本窗口内最大年龄
  double avg_ms = 0.0;         // 本窗口内平均年龄
};

struct UartCfg {
  bool enable = false;
  std::string device = "/dev/ttyS1";
  int baud = 115200;
  int rate_hz = 50;
  bool ascii = true;      // true=ASCII 帧；false=预留的二进制帧
};

class UartOut {
 public:
  UartOut() = default;
  ~UartOut();

  UartOut(const UartOut&) = delete;
  UartOut& operator=(const UartOut&) = delete;

  // 打开设备并启动发送线程。失败返回 false（不抛异常、不影响主流程）。
  bool start(const UartCfg& cfg);

  // 停止线程并关闭设备（可重复调用）
  void stop();

  // 主线程每帧调用：只做一次结构体拷贝 + 唤醒发送线程
  void publish(const TargetOut& out);
  // 带采集时间戳的重载：串口线程会在写之前重算「取帧→开始写 UART」年龄（仅诊断日志）
  void publish(const TargetOut& out, const FrameTiming& timing);

  // 诊断计数
  unsigned long sent_frames() const { return sent_frames_.load(); }
  unsigned long send_errors() const { return send_errors_.load(); }
  bool running() const { return running_.load(); }
  std::string last_error() const;

  // 读取并清零本统计窗口的「取帧→开始写 UART」年龄统计（串口线程写入，5 秒日志调用一次）
  WriteStartAgeStats take_write_start_age_stats();

 private:
  void set_snapshot(const TargetOut& out, const FrameTiming* timing);
  void add_write_start_age(int64_t age_us);
  void thread_main();
  bool open_device();
  void close_device();
  int write_all(const char* buf, int len);
  int format_ascii(const TargetOut& out, char* buf, int cap) const;

  UartCfg cfg_;
  int fd_ = -1;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};

  std::mutex mu_;
  UartSnapshot latest_{};
  bool has_latest_ = false;
  unsigned long published_seq_ = 0;

  // 「取帧→开始写 UART」年龄统计（只保留计数，不建历史队列）
  std::mutex write_age_mu_;
  unsigned long write_age_samples_ = 0;
  int64_t write_age_sum_us_ = 0;
  int write_age_last_ms_ = -1;
  int write_age_max_ms_ = -1;

  std::atomic<unsigned long> sent_frames_{0};
  std::atomic<unsigned long> send_errors_{0};

  mutable std::mutex err_mu_;
  std::string last_error_;
};

}  // namespace gt

#endif  // GREEN_TARGET_UART_OUT_H_
