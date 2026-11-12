// 串口输出：把跟踪结果按固定节奏发给电控
//
// 设计要点：
//   1. 独立线程发送，主线程 publish() 只做一次结构体拷贝（微秒级）
//   2. 固定频率发送（默认 50Hz），**即使没有目标也发**，让电控知道视觉在线
//   3. 默认 ASCII 格式（串口助手可直接看）；置 ascii=false 则发二进制 v2 帧
//      （28 字节 A5 5A ... CRC16，见 uart_frame.h 与 docs/UART_PROTOCOL_V2_DESIGN.md）
//   4. 打开失败不阻塞主流程：记录错误后静默降级，检测/网页不受影响
//   5. 快照同时携带该帧的采集时间戳（frame_timing.h）：串口线程在真正发送时
//      重新计算「取帧→发送」板端年龄，供诊断日志使用。ASCII 的 age 字段
//      仍是「距最近一次真实检测的毫秒数」，语义不变（见 docs/UART_PROTOCOL.md）。
//
// ASCII 帧格式（NMEA 风格，带 XOR 校验）：
//   $GT,<det>,<yaw>,<pitch>,<vyaw>,<vpitch>,<dist>,<age>,<seq>,<state>*<CS>\r\n
//   例：$GT,1,12.34,5.67,45.20,-12.30,3.456,0,1234,2*03
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
//   CS     正文的 XOR（两位大写十六进制）：从 'GT,' 之后第一个字符算到 '*' 之前，
//          **不含 'GT,'**（也不含 '$'）。算错这一范围是最常见的对接故障，见 docs/UART_PROTOCOL.md §2.3。
#ifndef GREEN_TARGET_UART_OUT_H_
#define GREEN_TARGET_UART_OUT_H_

#include <atomic>
#include <mutex>
#include <stdint.h>
#include <string>
#include <thread>

#include "frame_timing.h"
#include "target_track.h"
#include "uart_frame.h"

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
  bool ascii = true;      // true=ASCII v1 帧；false=二进制 v2 帧（28 字节，见 uart_frame.h）
  // 接收方向（电控 → 视觉，CMD=0x02）：只解析 + 打到终端，不参与任何计算
  bool rx_print = true;   // true=收到电控帧就往 stdout 打印（联调对照用）
  int rx_print_hz = 10;   // 打印限频，1..100（电控 100Hz 全打会刷屏）
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

  // ---- 接收方向（电控 → 视觉，CMD=0x02）的诊断接口 ----
  // 数据只在串口线程里更新；这里加锁拷贝，供主线程打日志用。
  bool has_mcu() const;
  McuState last_mcu() const;          // 最近一帧电控数据（未收到过时全 0，用 has_mcu() 判断）
  int mcu_age_ms() const;             // 距最近一帧电控数据的毫秒数；-1 = 从未收到
  unsigned long rx_frames() const;    // 收到的合法 0x02 帧数
  unsigned long rx_crc_errors() const;
  unsigned long rx_other_frames() const;   // 收到的其它 CMD 帧（例如回环的 0x01）

 private:
  void set_snapshot(const TargetOut& out, const FrameTiming* timing);
  void add_write_start_age(int64_t age_us);
  void thread_main();
  bool open_device();
  void close_device();
  int write_all(const char* buf, int len);
  int format_ascii(const TargetOut& out, char* buf, int cap) const;
  // 二进制 v2 帧：返回帧长（28），失败返回 0。帧序号用独立计数器（每发一帧 +1，0..255 回绕），
  // 不复用 TargetOut::seq（后者是「跟踪器状态更新次数」，允许跳变）。
  int format_v2(const TargetOut& out, uint8_t* buf, int cap);
  // 接收方向：非阻塞读完当前可读的字节并喂给解析器（不等待）
  void drain_rx();
  void on_mcu_frame(const McuState& m);   // 串口线程上下文
  void maybe_print_mcu(double now_ms);    // 限频打印到 stdout

  UartCfg cfg_;
  int fd_ = -1;
  // 终端副本：start.sh 会把 camera 的 stdout 重定向到日志文件，手动运行时用 /dev/tty
  // 把 [MCU] 行同时打到用户眼前（开不出来就只走 stdout）。
  int tty_fd_ = -1;
  bool tty_tried_ = false;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};

  std::mutex mu_;
  UartSnapshot latest_{};
  bool has_latest_ = false;
  unsigned long published_seq_ = 0;

  // 二进制 v2 帧的帧序号（只由发送线程读写，无需加锁）
  uint8_t frame_seq_ = 0;

  // 接收方向（电控 → 视觉）的状态；mcu_mu_ 保护下面这几个字段
  mutable std::mutex mcu_mu_;
  V2StreamParser rx_parser_;   // 只在串口线程里用，无需加锁
  McuState last_mcu_{};
  bool has_mcu_ = false;
  double last_mcu_ms_ = -1.0;      // 串口线程内使用
  double last_print_ms_ = -1.0;    // 打印限频
  unsigned long rx_frames_ = 0;
  unsigned long rx_crc_errors_ = 0;
  unsigned long rx_other_frames_ = 0;
  bool rx_announced_ = false;

  // 「取帧→开始写 UART」年龄统计（只保留计数，不建历史队列）
  std::mutex write_age_mu_;
  unsigned long write_age_samples_ = 0;
  int64_t write_age_sum_us_ = 0;
  int write_age_last_ms_ = -1;
  int write_age_max_ms_ = -1;

  std::atomic<unsigned long> sent_frames_{0};
  std::atomic<unsigned long> send_errors_{0};
  unsigned long tx_full_errors_ = 0;   // 发送缓冲满（EAGAIN）放弃次数
  unsigned long tx_hard_errors_ = 0;   // 其它写错误（EIO 等）次数

  mutable std::mutex err_mu_;
  std::string last_error_;
};

}  // namespace gt

#endif  // GREEN_TARGET_UART_OUT_H_
