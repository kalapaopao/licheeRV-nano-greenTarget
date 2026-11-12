// 串口输出实现，见 uart_out.h
#include "uart_out.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "uart_frame.h"

namespace gt {
namespace {

// 数值 → 波特率常量。板端 musl 的 termios 支持到 B4000000。
speed_t to_speed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return B115200;
  }
}

double mono_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

const char* state_code(TargetState s) {
  switch (s) {
    case TargetState::kLost: return "0";
    case TargetState::kCoasting: return "1";
    case TargetState::kConfirmed: return "2";
  }
  return "0";
}

}  // namespace

UartOut::~UartOut() { stop(); }

bool UartOut::start(const UartCfg& cfg) {
  cfg_ = cfg;
  if (!cfg_.enable) {
    return true;   // 未启用视为成功，但不启动线程
  }
  if (cfg_.baud <= 0) {
    cfg_.baud = 115200;
  }
  if (cfg_.rate_hz <= 0 || cfg_.rate_hz > 500) {
    cfg_.rate_hz = 50;
  }

  if (!open_device()) {
    return false;
  }

  quit_.store(false);
  running_.store(true);
  thread_ = std::thread([this]() { thread_main(); });
  return true;
}

void UartOut::stop() {
  if (!running_.load()) {
    return;
  }
  quit_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);
  close_device();
}

bool UartOut::open_device() {
  // O_NOCTTY：不要把串口变成控制终端（否则收到 ^C 会杀掉进程）
  // O_NONBLOCK：写不出去时立即返回，绝不阻塞主流程
  const int fd = ::open(cfg_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    std::lock_guard<std::mutex> g(err_mu_);
    last_error_ = std::string("open ") + cfg_.device + ": " + strerror(errno);
    fprintf(stderr, "UART: %s\n", last_error_.c_str());
    return false;
  }

  struct termios opt{};
  if (tcgetattr(fd, &opt) == 0) {
    cfmakeraw(&opt);                       // raw：不做任何字符转换
    const speed_t sp = to_speed(cfg_.baud);
    cfsetispeed(&opt, sp);
    cfsetospeed(&opt, sp);
    opt.c_cflag |= (CLOCAL | CREAD);       // 忽略调制解调器控制线，允许接收
    opt.c_cflag &= ~CRTSCTS;               // 不用硬件流控
    opt.c_cflag &= ~CSTOPB;                // 1 位停止位
    opt.c_cflag &= ~PARENB;                // 无校验
    opt.c_cflag &= ~CSIZE;
    opt.c_cflag |= CS8;                    // 8 位数据
    opt.c_cc[VMIN] = 0;
    opt.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &opt);
  }

  fd_ = fd;
  fprintf(stdout, "UART: %s @ %d 8N1, rate=%dHz, format=%s, rx_print=%s(%dHz)\n",
          cfg_.device.c_str(), cfg_.baud, cfg_.rate_hz,
          cfg_.ascii ? "ascii" : "binary-v2(28B)", cfg_.rx_print ? "on" : "off",
          cfg_.rx_print_hz);
  return true;
}

void UartOut::close_device() {
  if (tty_fd_ >= 0) {
    ::close(tty_fd_);
    tty_fd_ = -1;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int UartOut::write_all(const char* buf, int len) {
  if (fd_ < 0 || len <= 0) {
    return -1;
  }
  int off = 0;
  int guard = 0;
  while (off < len) {
    const ssize_t n = ::write(fd_, buf + off, static_cast<size_t>(len - off));
    if (n > 0) {
      off += static_cast<int>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // 发送缓冲满（对端没接/波特率不匹配）。最多重试 ~2ms 后放弃本帧，
      // 绝不死等，避免拖慢主流程或让发送线程卡住。
      if (++guard > 20) {
        ++tx_full_errors_;
        if (tx_full_errors_ <= 3 || (tx_full_errors_ % 500) == 0) {
          fprintf(stderr, "UART tx full: 连续写不出去（第 %lu 次），errno=EAGAIN\n",
                  tx_full_errors_);
          fflush(stderr);
        }
        return off;
      }
      struct timespec ts{0, 100000};   // 100us
      nanosleep(&ts, nullptr);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    // 其它错误（EIO 掉线、ENODEV 等）：打一次日志，便于现场定位
    ++tx_hard_errors_;
    if (tx_hard_errors_ <= 3) {
      fprintf(stderr, "UART write failed: errno=%d (%s)\n", errno, strerror(errno));
      fflush(stderr);
    }
    return off > 0 ? off : -1;
  }
  return off;
}

void UartOut::publish(const TargetOut& out) { set_snapshot(out, nullptr); }

void UartOut::publish(const TargetOut& out, const FrameTiming& timing) {
  set_snapshot(out, &timing);
}

void UartOut::set_snapshot(const TargetOut& out, const FrameTiming* timing) {
  if (!running_.load()) {
    return;
  }
  std::lock_guard<std::mutex> g(mu_);
  latest_.target = out;
  if (timing != nullptr) {
    latest_.timing = *timing;
    latest_.has_timing = true;
  } else {
    latest_.timing = FrameTiming();
    latest_.has_timing = false;
  }
  has_latest_ = true;
}

// 记录一次「取帧→开始写 UART」年龄样本（无效样本 age_us<0 只更新 last，不参与统计）
void UartOut::add_write_start_age(int64_t age_us) {
  const int age_ms = age_us < 0 ? -1 : static_cast<int>(age_us / 1000);
  std::lock_guard<std::mutex> g(write_age_mu_);
  write_age_last_ms_ = age_ms;
  if (age_us < 0) {
    return;
  }
  ++write_age_samples_;
  write_age_sum_us_ += age_us;
  if (age_ms > write_age_max_ms_) {
    write_age_max_ms_ = age_ms;
  }
}

WriteStartAgeStats UartOut::take_write_start_age_stats() {
  WriteStartAgeStats s;
  std::lock_guard<std::mutex> g(write_age_mu_);
  s.samples = write_age_samples_;
  s.last_ms = write_age_last_ms_;
  s.max_ms = write_age_max_ms_;
  if (write_age_samples_ > 0) {
    s.avg_ms = static_cast<double>(write_age_sum_us_) / 1000.0 /
               static_cast<double>(write_age_samples_);
  }
  write_age_samples_ = 0;
  write_age_sum_us_ = 0;
  write_age_max_ms_ = -1;
  return s;
}

// 组装 ASCII 帧，返回长度（不含结尾 '\0'）
int UartOut::format_ascii(const TargetOut& out, char* buf, int cap) const {
  if (buf == nullptr || cap < 32) {
    return 0;
  }

  char body[160];
  const float age = (out.age_ms < 0.0f) ? -1.0f : out.age_ms;
  const int n = snprintf(body, sizeof(body), "%d,%.2f,%.2f,%.2f,%.2f,%.3f,%.0f,%lu,%s",
                         out.detected ? 1 : 0, out.yaw_deg, out.pitch_deg, out.v_yaw_dps,
                         out.v_pitch_dps, out.distance_valid ? out.distance_m : 0.0f,
                         age, out.seq, state_code(out.state));
  if (n <= 0 || n >= static_cast<int>(sizeof(body))) {
    return 0;
  }

  unsigned char cs = 0;
  for (int i = 0; i < n; ++i) {
    cs ^= static_cast<unsigned char>(body[i]);
  }

  const int written = snprintf(buf, static_cast<size_t>(cap), "$GT,%s*%02X\r\n", body, cs);
  if (written <= 0 || written >= cap) {
    return 0;
  }
  return written;
}

// 组装二进制 v2 帧（28 字节），返回帧长；失败返回 0。
// 数据区规则（detected==0 时角度清零、dist=-1.0）在 v2_make_data() 里，可在主机上单测；
// 帧序号用本类自己的计数器，不复用 TargetOut::seq（那是「跟踪器更新次数」，会跳变）。
int UartOut::format_v2(const TargetOut& out, uint8_t* buf, int cap) {
  const V2Data d = v2_make_data(out.detected, out.yaw_deg, out.pitch_deg, out.v_yaw_dps,
                                out.v_pitch_dps, out.distance_valid, out.distance_m);
  const int n = build_v2_frame(d, frame_seq_, buf, cap);
  if (n > 0) {
    ++frame_seq_;   // uint8_t 自然 0..255 回绕
  }
  return n;
}

void UartOut::thread_main() {
  const int period_us = 1000000 / (cfg_.rate_hz > 0 ? cfg_.rate_hz : 50);
  double next_ms = mono_ms();

  char frame[192];
  while (!quit_.load()) {
    UartSnapshot snapshot;
    bool has = false;
    {
      std::lock_guard<std::mutex> g(mu_);
      if (has_latest_) {
        snapshot = latest_;
        has = true;
      }
    }

    if (has) {
      // 采样点在 write_all() 之前，所以这里得到的是「取帧→**开始写** UART」的年龄
      // （日志字段名带 write_start，避免被误读成写完成）。它与 TargetOut::age_ms
      // （跟踪器内部的数据陈旧度）是两个不同的量：后者进 ASCII 的 age 字段（语义不变），
      // 前者只进诊断日志。
      const MonoUs write_start_us = monotonic_us();
      add_write_start_age(
          frame_age_us(snapshot.has_timing, snapshot.timing.capture_mono_us, write_start_us));

      const int len = cfg_.ascii
                          ? format_ascii(snapshot.target, frame, sizeof(frame))
                          : format_v2(snapshot.target, reinterpret_cast<uint8_t*>(frame),
                                      sizeof(frame));
      if (len > 0) {
        const int w = write_all(frame, len);
        if (w == len) {
          ++sent_frames_;
        } else {
          ++send_errors_;
        }
      }
    }

    // 收电控帧：非阻塞抽一次（不改变原来的发送节奏）
    drain_rx();

    // 固定节奏：用绝对时间推进，避免累积漂移（与加接收前完全一致）
    next_ms += static_cast<double>(period_us) / 1000.0;
    const double now = mono_ms();
    if (next_ms < now) {
      next_ms = now;   // 落后了就不补发，直接对齐到当前
    }
    const long sleep_us = static_cast<long>((next_ms - now) * 1000.0);
    if (sleep_us <= 0) {
      continue;
    }
    // 睡眠期间用 poll 等"串口可读"：可读就立刻起来收帧，然后补足剩余睡眠时间。
    // 这样接收延迟最长 1 个发送周期（20ms@50Hz），但发送节奏完全不受影响。
    const double deadline = next_ms;
    if (fd_ >= 0) {
      struct pollfd p{};
      p.fd = fd_;
      p.events = POLLIN;
      const int wait_ms = static_cast<int>(sleep_us / 1000);
      if (wait_ms > 0 && poll(&p, 1, wait_ms) > 0) {
        drain_rx();
      }
    }
    const double rest = deadline - mono_ms();
    if (rest > 0.0) {
      const long rest_us = static_cast<long>(rest * 1000.0);
      struct timespec ts{static_cast<time_t>(rest_us / 1000000),
                         static_cast<long>((rest_us % 1000000) * 1000)};
      nanosleep(&ts, nullptr);
    }
  }
}

// ---------------------------------------------------------------------------
// 接收方向：电控 → 视觉（CMD=0x02）。只解析 + 打印，不参与任何计算。
// 非阻塞：有多少读多少，绝不等待（等待由 thread_main 的 poll+nanosleep 负责）。
// ---------------------------------------------------------------------------
void UartOut::drain_rx() {
  if (fd_ < 0) {
    return;
  }
  uint8_t buf[512];
  for (int round = 0; round < 16; ++round) {
    const ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    for (ssize_t i = 0; i < n; ++i) {
      McuState m;
      const V2StreamParser::FeedResult r = rx_parser_.feed(buf[i], &m);
      if (r == V2StreamParser::kMcuFrame) {
        on_mcu_frame(m);
      }
    }
  }
  // 统计计数与解析结果无关（CRC 错、其它 CMD 也会变），每次抽完同步一次
  {
    std::lock_guard<std::mutex> g(mcu_mu_);
    rx_frames_ = rx_parser_.frames();
    rx_crc_errors_ = rx_parser_.crc_errors();
    rx_other_frames_ = rx_parser_.other_cmd();
  }
}

void UartOut::on_mcu_frame(const McuState& m) {
  const double now = mono_ms();
  {
    std::lock_guard<std::mutex> g(mcu_mu_);
    last_mcu_ = m;
    has_mcu_ = true;
    last_mcu_ms_ = now;
    rx_frames_ = rx_parser_.frames();
    rx_crc_errors_ = rx_parser_.crc_errors();
    rx_other_frames_ = rx_parser_.other_cmd();
  }

  if (!rx_announced_) {
    rx_announced_ = true;
    if (!tty_tried_) {
      tty_tried_ = true;
      tty_fd_ = ::open("/dev/tty", O_WRONLY | O_NOCTTY | O_NONBLOCK);
    }
    printf("[MCU] 收到电控数据（CMD=0x02，33 字节帧），下面按 %dHz 打印\n", cfg_.rx_print_hz);
    fflush(stdout);
  }
  maybe_print_mcu(now);
}

void UartOut::maybe_print_mcu(double now_ms) {
  if (!cfg_.rx_print) {
    return;
  }
  const int hz = (cfg_.rx_print_hz > 0 && cfg_.rx_print_hz <= 100) ? cfg_.rx_print_hz : 10;
  const double period = 1000.0 / static_cast<double>(hz);
  if (last_print_ms_ >= 0.0 && (now_ms - last_print_ms_) < period) {
    return;
  }
  last_print_ms_ = now_ms;

  const McuState& m = last_mcu_;
  const char* st = (m.state == kMcuStateFlight)   ? "FLIGHT"
                   : (m.state == kMcuStateStop) ? "STOP"
                                                : "STATIC";
  const int age_ms = (last_mcu_ms_ < 0.0) ? -1 : static_cast<int>(now_ms - last_mcu_ms_);
  char line[256];
  const int n = snprintf(line, sizeof(line),
                         "[MCU] seq=%3u state=%s flags=0x%02X(imu=%d bias=%d acc=%d) "
                         "roll=%7.2f pitch=%7.2f yaw=%8.2f | gx=%7.2f gy=%7.2f gz=%7.2f | age=%dms\n",
                         static_cast<unsigned>(m.seq), st, static_cast<unsigned>(m.flags),
                         m.imu_valid() ? 1 : 0, (m.flags & kMcuFlagBiasReady) ? 1 : 0,
                         (m.flags & kMcuFlagAccValid) ? 1 : 0, m.roll, m.pitch, m.yaw, m.gx, m.gy,
                         m.gz, age_ms);
  if (n <= 0) {
    return;
  }
  fputs(line, stdout);   // 进日志文件（start.sh 重定向）
  fflush(stdout);        // 输出被重定向到日志文件时也要能立刻看到
  if (tty_fd_ >= 0) {
    const ssize_t w = ::write(tty_fd_, line, static_cast<size_t>(n));   // 同时打到终端
    (void)w;
  }
}

bool UartOut::has_mcu() const {
  std::lock_guard<std::mutex> g(mcu_mu_);
  return has_mcu_;
}

McuState UartOut::last_mcu() const {
  std::lock_guard<std::mutex> g(mcu_mu_);
  return last_mcu_;
}

int UartOut::mcu_age_ms() const {
  std::lock_guard<std::mutex> g(mcu_mu_);
  if (!has_mcu_ || last_mcu_ms_ < 0.0) {
    return -1;
  }
  return static_cast<int>(mono_ms() - last_mcu_ms_);
}

unsigned long UartOut::rx_frames() const {
  std::lock_guard<std::mutex> g(mcu_mu_);
  return rx_frames_;
}

unsigned long UartOut::rx_crc_errors() const {
  std::lock_guard<std::mutex> g(mcu_mu_);
  return rx_crc_errors_;
}

unsigned long UartOut::rx_other_frames() const {
  std::lock_guard<std::mutex> g(mcu_mu_);
  return rx_other_frames_;
}

std::string UartOut::last_error() const {
  std::lock_guard<std::mutex> g(err_mu_);
  return last_error_;
}

}  // namespace gt
