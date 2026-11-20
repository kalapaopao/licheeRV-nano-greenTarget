// 飞行数据记录：每帧一行 CSV + 电控每帧一行 CSV，带预录环形缓冲（T+ 触发方案）
//
// 为什么长这样（设计约束，改之前先读）：
//   1. 触发源只有电控 CMD=0x02 的 STATE 边沿（STATIC→FLIGHT 起飞、FLIGHT→STOP 落地），
//      协议见 docs/UART_PROTOCOL_V2_DESIGN.md 与电控的《视觉串口通信协议》§4.2。
//      视觉侧没有 IMU，无法自己判起飞。
//   2. 触发前必须有预录：电控判定 + 10 ms 发送 + 视觉接收，飞机已经离手几十毫秒，
//      「收到信号才 open 文件」必然丢开头。所以行先进**环形缓冲**，触发时整体落盘。
//   3. 采集线程绝不碰文件系统：on_frame() 只做一次 snprintf + memcpy（~5 µs），
//      真正的 write/fdatasync 在独立写线程里做。80 Hz 主循环不能被 I/O 拖慢。
//   4. 时间基准是 CLOCK_MONOTONIC 微秒（板子没有 RTC，墙钟不可信）：跨流对齐靠时间戳，
//      不靠 SEQ（协议 §4.5 明确说 0x02 没有时间戳、也不回显视觉序号）。
//
// 产物（每个会话一个目录）：
//   <log_dir>/sess_<uptime>s_<pid>_<n>/frames.csv   相机每帧一行
//   <log_dir>/sess_<uptime>s_<pid>_<n>/mcu.csv      电控每帧一行（收到的全部，不抽样）
//   <log_dir>/sess_<uptime>s_<pid>_<n>/meta.txt     触发时刻、配置快照、结束原因、丢行数
//   <log_dir>/health_<uptime>s_<pid>.csv            每秒一行，答「为什么这场没记到」
//   <log_dir>/video_frames_<uptime>s_<pid>.csv      AVI 帧号 ↔ 相机帧号（录像与 CSV 对帧用）
#ifndef GREEN_TARGET_FRAME_LOG_H_
#define GREEN_TARGET_FRAME_LOG_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame_timing.h"
#include "target_track.h"
#include "uart_frame.h"

namespace gt {

struct LogCfg {
  bool enable = false;
  bool trigger_mode = true;   // true=T+（MCU 起飞边沿触发，带预录）；false=连续记录
  // 是否额外保存 100Hz 的原始电控流（mcu.csv）。
  // 默认 false：frames.csv 每一行已经带上该帧时刻的电控 6 个浮点 + state/flags/age，
  // 单个文件就是"一帧一行"的成品表；只有想看两帧之间的完整 100Hz 波形时才需要打开。
  bool mcu_stream = false;
  std::string dir = "logs";
  int preroll_ms = 3000;      // 预录窗口（只在 trigger 模式有意义）
  int postroll_ms = 2000;     // 落地后继续记多久（收撞击后的尾声）
  int max_seconds = 30;       // 单会话上限：防止一直不离开 FLIGHT 时写满卡
  int ring_rows = 2048;       // 环形缓冲行数；180 行/秒时约 11 秒
};

struct LogStats {
  unsigned long sessions = 0;      // 已经开始的会话数（= 起飞次数）
  unsigned long rows_frames = 0;   // 已落盘帧行数
  unsigned long rows_mcu = 0;      // 已落盘电控行数
  unsigned long rows_video = 0;    // 视频侧车行数
  unsigned long dropped = 0;       // 会话内缓冲溢出丢的行数（>0 说明写盘跟不上）
  unsigned long triggers = 0;      // 收到的起飞边沿次数
  int open = 0;                    // 当前是否有会话在写
  int last_session_ms = -1;        // 距最近一次会话开/关的毫秒数
};

class FrameLog {
 public:
  FrameLog();
  ~FrameLog();

  FrameLog(const FrameLog&) = delete;
  FrameLog& operator=(const FrameLog&) = delete;

  // 建目录 + 起写线程。enable=false 时直接返回 true 且不起线程（所有 on_* 变空操作）。
  bool start(const LogCfg& cfg);
  void stop();
  bool running() const { return running_.load(); }
  // 是否是「触发式」（决定录像要不要跟会话走：触发式才会话里另起一个 video.avi）
  bool trigger_mode() const { return cfg_.trigger_mode; }
  // 会话正在录时，返回该会话目录（视频就写在这里，和数据同一个文件夹）。
  // 返回 false 表示当前没有会话（没开始/已结束）。
  bool session_video_dir(std::string* out) const;

  // start() 之前调用：这些行原样写进每个会话的 meta.txt（配置快照/版本号）
  void add_meta(const std::string& line);

  // ---- 生产者：都可从任意线程调用，内部只做格式化 + 入队，不做 I/O ----
  // 相机线程：每帧一次。ran_detect=0 表示这一帧没跑检测（stride/开关），只记时间戳。
  // mcu_rx_us = 该帧所用的那帧电控数据的「绝对接收时刻」（0 = 无），
  // 和 capture_us 一起写进同一行 —— 一行里两个时刻都写明，事后不用推算。
  void on_frame(const FrameTiming& t, const TargetOut& out, int ran_detect, int grid_s,
                const McuState* mcu, int mcu_age_ms, MonoUs mcu_rx_us);
  // 串口接收线程（UartOut 的回调）：每收到一帧 CMD=0x02 一次。rx_us 是本地接收时刻。
  void on_mcu(const McuState& m, MonoUs rx_us);
  // 编码线程：每成功写进 AVI 一帧一次（frame_id/capture_us 来自当时的采集帧）
  void on_video(unsigned long avi_index, uint32_t frame_id, MonoUs capture_us, uint64_t pts_raw);
  // 相机线程：每秒一次健康行（写 health_*.csv）
  void on_health(double fps, unsigned long frames, unsigned long encoded, unsigned long mcu_rx,
                 int mcu_age_ms);

  LogStats stats() const;
  std::string last_error() const;

 private:
  static const int kRowMax = 256;      // 单行文本上限（含 '\n'）
  static const int kBatch = 64;        // 一次取多少行
  // 「本会话还没有任何 AVI 帧」的哨兵值（AVI 帧号 0 是合法的）
  static const unsigned long kAviNone = ~0UL;
  struct Row {
    uint16_t len;
    uint8_t kind;                      // 1=frames.csv 2=mcu.csv
    uint64_t ts_us;                    // 该行自己的时刻（用于丢弃超出预录窗口的旧行）
    char text[kRowMax];
    Row() : len(0), kind(0), ts_us(0) { text[0] = 0; }
  };

  void thread_main();
  bool push(uint8_t kind, const char* text, int len, uint64_t ts_us);   // 入队（加锁）
  void poll_lifecycle();                               // 写线程：处理触发/落地/超时
  void open_session(MonoUs trigger_us);
  void close_session(const char* reason);
  void write_rows(const std::vector<Row>& rows);
  void sync_all();
  int open_append(const std::string& path, bool truncate);
  bool make_dir(const std::string& path);
  void set_error(const std::string& what);

  LogCfg cfg_;
  bool configured_ = false;
  std::string dir_;
  std::vector<std::string> meta_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Row> ring_;
  int cap_ = 0;
  int head_ = 0;
  int tail_ = 0;
  int count_ = 0;

  // 写线程私有（不带锁）
  bool session_open_ = false;
  MonoUs session_start_us_ = 0;
  MonoUs preroll_floor_us_ = 0;     // 低于该时刻的行属于「超出预录窗口」，落盘时丢弃
  unsigned long session_index_ = 0;
  std::string session_dir_;
  int frames_fd_ = -1;
  int mcu_fd_ = -1;
  int meta_fd_ = -1;
  unsigned long sess_rows_frames_ = 0;
  unsigned long sess_rows_mcu_ = 0;
  unsigned long sess_dropped_ = 0;
  unsigned long handled_trigger_ = 0;
  MonoUs last_session_end_us_ = 0;
  bool land_seen_ = false;          // 写线程是否已经看到落地边沿
  MonoUs land_seen_us_ = 0;         // 看到落地边沿的本地时刻（postroll 从这里开始算）

  // 触发/落地信号（生产者写，写线程读）
  std::atomic<unsigned long> trigger_seq_{0};
  std::atomic<uint64_t> trigger_us_{0};
  std::atomic<uint64_t> land_us_{0};
  std::atomic<int> last_mcu_state_{-1};

  // 辅助文件（健康行 / 视频侧车）：相机线程与编码线程直接追加
  mutable std::mutex aux_mu_;
  int health_fd_ = -1;
  int video_fd_ = -1;        // 全程侧车（没有会话时用）
  int sess_video_fd_ = -1;   // 会话内侧车：<会话目录>/video_frames.csv

  // 统计（写线程与相机线程都会读）
  std::atomic<unsigned long> sessions_{0};
  std::atomic<unsigned long> rows_frames_{0};
  std::atomic<unsigned long> rows_mcu_{0};
  std::atomic<unsigned long> rows_video_{0};
  std::atomic<unsigned long> dropped_{0};
  std::atomic<unsigned long> triggers_{0};
  std::atomic<unsigned long> session_open_flag_{0};
  std::atomic<uint64_t> last_session_event_us_{0};
  std::atomic<unsigned long> io_errors_{0};
  // 本会话对应的视频区间（AVI 是全程连续录的，这里记下「这次飞行落在 AVI 的第几帧到第几帧」）
  std::atomic<unsigned long> sess_avi_first_{kAviNone};
  std::atomic<unsigned long> sess_avi_last_{kAviNone};
  std::atomic<unsigned long> sess_avi_rows_{0};

  mutable std::mutex err_mu_;
  std::string last_error_;
};

}  // namespace gt

#endif  // GREEN_TARGET_FRAME_LOG_H_
