// 串口输出实现，见 uart_out.h
#include "uart_out.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

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
  fprintf(stdout, "UART: %s @ %d 8N1, rate=%dHz, format=%s\n", cfg_.device.c_str(),
          cfg_.baud, cfg_.rate_hz, cfg_.ascii ? "ascii" : "binary");
  return true;
}

void UartOut::close_device() {
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
        return off;
      }
      struct timespec ts{0, 100000};   // 100us
      nanosleep(&ts, nullptr);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
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

      const int len = format_ascii(snapshot.target, frame, sizeof(frame));
      if (len > 0) {
        const int w = write_all(frame, len);
        if (w == len) {
          ++sent_frames_;
        } else {
          ++send_errors_;
        }
      }
    }

    // 固定节奏：用绝对时间推进，避免累积漂移
    next_ms += static_cast<double>(period_us) / 1000.0;
    const double now = mono_ms();
    if (next_ms < now) {
      next_ms = now;   // 落后了就不补发，直接对齐到当前
    }
    const long sleep_us = static_cast<long>((next_ms - now) * 1000.0);
    if (sleep_us > 0) {
      struct timespec ts{static_cast<time_t>(sleep_us / 1000000),
                         static_cast<long>((sleep_us % 1000000) * 1000)};
      nanosleep(&ts, nullptr);
    }
  }
}

std::string UartOut::last_error() const {
  std::lock_guard<std::mutex> g(err_mu_);
  return last_error_;
}

}  // namespace gt
