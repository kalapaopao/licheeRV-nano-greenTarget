// 飞行数据记录实现，见 frame_log.h
//
// 线程模型：
//   相机线程 ──on_frame()──┐
//   串口线程 ──on_mcu()────┤→ 环形缓冲（mutex + condvar）→ 写线程 → files/fdatasync
//   编码线程 ──on_video()──┘（on_video/on_health 走独立 fd，不占环形缓冲）
//
// 为什么 mcu 行也要进缓冲：预录窗口里必须同时有「帧」和「电控状态」，
// 否则触发前的 3 秒只有画面数字、没有姿态，事后没法回答「起飞前云台在哪」。
#include "frame_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <chrono>

#include "diag.h"

namespace gt {
namespace {

const int kTickMs = 20;              // 写线程最长睡眠（决定触发/落地判定的时间粒度）
const int kSyncMs = 1000;            // fdatasync 周期

double mono_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

// 墙钟只写进 meta 供参考；板子没有 RTC 时可能是 1970，不要用它做排序。
std::string wall_text() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv {};
  if (localtime_r(&ts.tv_sec, &tmv) == nullptr) {
    return "unknown";
  }
  char buf[64];
  if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0) {
    return "unknown";
  }
  char out[80];
  snprintf(out, sizeof(out), "%s.%03ld", buf, static_cast<long>(ts.tv_nsec / 1000000));
  return out;
}

// 进程启动以来的秒数：板子没 RTC，用它给文件/目录起名，天然可排序且不重复。
unsigned long uptime_s() {
  return static_cast<unsigned long>(monotonic_us() / 1000000ULL);
}

void put_fd(int fd, const char* p, int n, int* err) {
  int off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, p + off, static_cast<size_t>(n - off));
    if (w > 0) {
      off += static_cast<int>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) {
      continue;
    }
    ++(*err);
    return;
  }
}

}  // namespace

FrameLog::FrameLog() = default;

FrameLog::~FrameLog() { stop(); }

void FrameLog::set_error(const std::string& what) {
  std::lock_guard<std::mutex> g(err_mu_);
  last_error_ = what;
}

std::string FrameLog::last_error() const {
  std::lock_guard<std::mutex> g(err_mu_);
  return last_error_;
}

bool FrameLog::make_dir(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
    return true;
  }
  return false;
}

int FrameLog::open_append(const std::string& path, bool truncate) {
  int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
  flags |= truncate ? O_TRUNC : O_APPEND;
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    set_error(std::string("open ") + path + ": " + strerror(errno));
  }
  return fd;
}

void FrameLog::add_meta(const std::string& line) {
  if (running()) {
    return;   // start() 之后不再接受（避免和写线程抢 meta_fd_）
  }
  meta_.push_back(line);
}

bool FrameLog::start(const LogCfg& cfg) {
  cfg_ = cfg;
  if (!cfg_.enable) {
    configured_ = true;
    running_.store(false);
    return true;
  }
  if (cfg_.ring_rows < 256) {
    cfg_.ring_rows = 256;
  }
  if (cfg_.ring_rows > 20000) {
    cfg_.ring_rows = 20000;
  }
  if (cfg_.postroll_ms < 0) {
    cfg_.postroll_ms = 0;
  }
  if (cfg_.preroll_ms < 0) {
    cfg_.preroll_ms = 0;
  }
  if (cfg_.max_seconds <= 0) {
    cfg_.max_seconds = 30;
  }

  dir_ = cfg_.dir.empty() ? std::string("logs") : cfg_.dir;
  if (!make_dir(dir_)) {
    set_error(std::string("mkdir ") + dir_ + ": " + strerror(errno));
    fprintf(stderr, "LOG: %s\n", last_error().c_str());
    return false;
  }

  ring_.assign(static_cast<size_t>(cfg_.ring_rows), Row());
  cap_ = cfg_.ring_rows;
  head_ = tail_ = count_ = 0;

  // 健康行/视频侧车：整场只开一次，不随会话开关
  char name[64];
  snprintf(name, sizeof(name), "/health_%lus_%ld.csv", uptime_s(), static_cast<long>(getpid()));
  health_fd_ = open_append(dir_ + name, true);
  if (health_fd_ < 0) {
    return false;
  }
  const char* hh =
      "mono_us,fps,frames,encoded,mcu_rx,mcu_age_ms,session_open,dropped\n";
  int err = 0;
  put_fd(health_fd_, hh, static_cast<int>(strlen(hh)), &err);

  snprintf(name, sizeof(name), "/video_frames_%lus_%ld.csv", uptime_s(),
           static_cast<long>(getpid()));
  video_fd_ = open_append(dir_ + name, true);
  if (video_fd_ < 0) {
    return false;
  }
  const char* vh = "avi_index,frame_id,capture_us,pts_raw\n";
  put_fd(video_fd_, vh, static_cast<int>(strlen(vh)), &err);

  quit_.store(false);
  running_.store(true);
  thread_ = std::thread([this]() { thread_main(); });

  if (gt::diag(gt::kDiagLog)) {
    fprintf(stdout, "LOG: dir=%s mode=%s preroll=%dms postroll=%dms max=%ds ring=%d rows\n",
            dir_.c_str(), cfg_.trigger_mode ? "trigger(mcu FLIGHT edge)" : "continuous",
            cfg_.preroll_ms, cfg_.postroll_ms, cfg_.max_seconds, cfg_.ring_rows);
    fflush(stdout);
  }
  return true;
}

void FrameLog::stop() {
  if (!running_.load()) {
    return;
  }
  quit_.store(true);
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);

  std::lock_guard<std::mutex> g(aux_mu_);
  if (health_fd_ >= 0) {
    ::fdatasync(health_fd_);
    ::close(health_fd_);
    health_fd_ = -1;
  }
  if (video_fd_ >= 0) {
    ::fdatasync(video_fd_);
    ::close(video_fd_);
    video_fd_ = -1;
  }
}

// ---------------------------------------------------------------------------
// 生产者
// ---------------------------------------------------------------------------
bool FrameLog::push(uint8_t kind, const char* text, int len, uint64_t ts_us) {
  if (!running_.load() || len <= 0) {
    return false;
  }
  if (len >= kRowMax) {
    len = kRowMax - 1;
  }
  std::lock_guard<std::mutex> g(mu_);
  if (count_ == cap_) {
    if (session_open_flag_.load()) {
      // 会话进行中：丢新行，保证已落盘的是连续前缀（丢在尾部，不是中间挖洞）
      dropped_.fetch_add(1);
      return false;
    }
    // 空闲（预录）：丢最旧，保证环形缓冲里始终是最近的 N 行
    tail_ = (tail_ + 1) % cap_;
    --count_;
  }
  Row& r = ring_[static_cast<size_t>(head_)];
  r.kind = kind;
  r.len = static_cast<uint16_t>(len);
  r.ts_us = ts_us;
  memcpy(r.text, text, static_cast<size_t>(len));
  r.text[len] = 0;
  head_ = (head_ + 1) % cap_;
  ++count_;
  cv_.notify_one();
  return true;
}

void FrameLog::on_frame(const FrameTiming& t, const TargetOut& out, int ran_detect, int grid_s,
                        const McuState* mcu, int mcu_age_ms, MonoUs mcu_rx_us) {
  if (!running_.load()) {
    return;
  }
  const int state = static_cast<int>(out.state);
  const float gs = grid_s > 0 ? static_cast<float>(grid_s) : 1.0f;
  const int mc_state = mcu != nullptr ? static_cast<int>(mcu->state) : -1;
  const int mc_flags = mcu != nullptr ? static_cast<int>(mcu->flags) : -1;
  // 电控的 6 个浮点直接并进这一行：这样单文件就是"一帧一行"的成品表，
  // 不需要事后再拿 mcu.csv 去配对。没有电控数据时留空（pandas 会读成 NaN）。
  char mcu_fields[96];
  if (mcu != nullptr) {
    snprintf(mcu_fields, sizeof(mcu_fields), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
             mcu->roll, mcu->pitch, mcu->yaw, mcu->gx, mcu->gy, mcu->gz);
  } else {
    // 没有电控数据：仍然写满 6 个字段（nan 能被 pandas/Excel 识别为缺失），
    // 保证每行列数恒定，下游按列索引不会错位
    snprintf(mcu_fields, sizeof(mcu_fields), "nan,nan,nan,nan,nan,nan");
  }
  char mcu_rx[24];
  if (mcu != nullptr && mcu_rx_us != 0) {
    snprintf(mcu_rx, sizeof(mcu_rx), "%llu",
             static_cast<unsigned long long>(mcu_rx_us));
  } else {
    snprintf(mcu_rx, sizeof(mcu_rx), "nan");
  }
  char line[kRowMax];
  const int n = snprintf(
      line, sizeof(line),
      "%u,%llu,%llu,%d,%u,%u,%d,%d,%lu,%.1f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,"
      "%.1f,%.1f,%.1f,%d,%d,%d,%d,%s,%s\n",
      static_cast<unsigned>(t.frame_id),
      static_cast<unsigned long long>(t.capture_mono_us),
      static_cast<unsigned long long>(t.vi_pts_raw), ran_detect,
      static_cast<unsigned>(t.detect_duration_us()),
      static_cast<unsigned>(t.capture_latency_us()),
      out.detected ? 1 : 0, state, out.seq, out.age_ms, out.candidates,
      out.yaw_deg, out.pitch_deg, out.v_yaw_dps, out.v_pitch_dps, out.distance_m,
      out.distance_valid ? 1 : 0, out.light_yaw_deg, out.light_pitch_deg,
      out.gx * gs, out.gy * gs, out.gr * gs, out.has_box ? 1 : 0,
      mc_state, mc_flags, mcu_age_ms, mcu_rx, mcu_fields);
  if (n <= 0) {
    return;
  }
  push(1, line, n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1,
       t.capture_mono_us);
}

void FrameLog::on_mcu(const McuState& m, MonoUs rx_us) {
  if (!running_.load()) {
    return;
  }
  // 边沿判定只在这里做：串口线程是 0x02 的唯一生产者，所以 last_mcu_state_ 无竞争。
  const int prev = last_mcu_state_.exchange(static_cast<int>(m.state));
  if (m.state == kMcuStateFlight && prev != static_cast<int>(kMcuStateFlight)) {
    // 起飞（含「程序启动时已经在飞」的情况：prev=-1 也触发，宁多记不漏记）
    trigger_us_.store(rx_us);
    trigger_seq_.fetch_add(1);
    triggers_.fetch_add(1);
  } else if (prev == static_cast<int>(kMcuStateFlight) &&
             m.state != static_cast<int>(kMcuStateFlight)) {
    land_us_.store(rx_us);
  }

  // 只有显式打开 mcu_stream 才把 100Hz 原始流也存一份；
  // 默认关：frames.csv 每行已经带该帧时刻的电控数据，单文件就够了。
  if (!cfg_.mcu_stream) {
    return;
  }
  char line[kRowMax];
  const int n = snprintf(line, sizeof(line), "%llu,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                         static_cast<unsigned long long>(rx_us), static_cast<unsigned>(m.seq),
                         static_cast<unsigned>(m.state), static_cast<unsigned>(m.flags), m.roll,
                         m.pitch, m.yaw, m.gx, m.gy, m.gz);
  if (n > 0) {
    push(2, line, n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1,
         rx_us);
  }
}

void FrameLog::on_video(unsigned long avi_index, uint32_t frame_id, MonoUs capture_us,
                        uint64_t pts_raw) {
  if (!running_.load()) {
    return;
  }
  char line[128];
  const int n = snprintf(line, sizeof(line), "%lu,%u,%llu,%llu\n", avi_index,
                         static_cast<unsigned>(frame_id),
                         static_cast<unsigned long long>(capture_us),
                         static_cast<unsigned long long>(pts_raw));
  if (n <= 0) {
    return;
  }
  std::lock_guard<std::mutex> g(aux_mu_);
  // 会话进行中：侧车写进会话目录（和 video.avi、frames.csv 放在一起），并记下区间
  int fd = video_fd_;
  if (session_open_flag_.load() != 0 && sess_video_fd_ >= 0) {
    fd = sess_video_fd_;
    const unsigned long none = kAviNone;
    unsigned long first = sess_avi_first_.load();
    while (avi_index < first && !sess_avi_first_.compare_exchange_weak(first, avi_index)) {
      first = sess_avi_first_.load();
    }
    sess_avi_last_.store(avi_index);
    sess_avi_rows_.fetch_add(1);
  }
  (void)kAviNone;
  if (fd < 0) {
    return;
  }
  int err = 0;
  put_fd(fd, line, n, &err);
  if (err) {
    io_errors_.fetch_add(static_cast<unsigned long>(err));
  }
  rows_video_.fetch_add(1);
}

void FrameLog::on_health(double fps, unsigned long frames, unsigned long encoded,
                         unsigned long mcu_rx, int mcu_age_ms) {
  if (!running_.load() || health_fd_ < 0) {
    return;
  }
  char line[160];
  const int n = snprintf(line, sizeof(line), "%llu,%.2f,%lu,%lu,%lu,%d,%d,%lu\n",
                         static_cast<unsigned long long>(monotonic_us()), fps, frames, encoded,
                         mcu_rx, mcu_age_ms, session_open_flag_.load() ? 1 : 0,
                         dropped_.load());
  if (n <= 0) {
    return;
  }
  std::lock_guard<std::mutex> g(aux_mu_);
  if (health_fd_ < 0) {
    return;
  }
  int err = 0;
  put_fd(health_fd_, line, n, &err);
  if (err) {
    io_errors_.fetch_add(static_cast<unsigned long>(err));
  }
}

// ---------------------------------------------------------------------------
// 写线程
// ---------------------------------------------------------------------------
void FrameLog::write_rows(const std::vector<Row>& rows) {
  int err = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    const bool is_frame = (r.kind == 1);
    const int fd = is_frame ? frames_fd_ : mcu_fd_;
    if (fd < 0) {
      continue;
    }
    put_fd(fd, r.text, r.len, &err);
    if (is_frame) {
      ++sess_rows_frames_;
      rows_frames_.fetch_add(1);
    } else {
      ++sess_rows_mcu_;
      rows_mcu_.fetch_add(1);
    }
  }
  if (err) {
    io_errors_.fetch_add(static_cast<unsigned long>(err));
    set_error("session write failed");
  }
}

void FrameLog::open_session(MonoUs trigger_us) {
  ++session_index_;
  char name[96];
  snprintf(name, sizeof(name), "/sess_%lus_%ld_%03lu", uptime_s(), static_cast<long>(getpid()),
           session_index_);
  session_dir_ = dir_ + name;
  if (!make_dir(session_dir_)) {
    set_error(std::string("mkdir ") + session_dir_ + ": " + strerror(errno));
    return;
  }
  frames_fd_ = open_append(session_dir_ + "/frames.csv", true);
  // mcu.csv 是可选的：默认不建，frames.csv 每行已经带电控数据
  mcu_fd_ = cfg_.mcu_stream ? open_append(session_dir_ + "/mcu.csv", true) : -1;
  meta_fd_ = open_append(session_dir_ + "/meta.txt", true);
  // 会话内侧车：这一段的 AVI 帧号 ↔ frame_id（AVI 也是本会话专属的 video.avi）
  sess_video_fd_ = open_append(session_dir_ + "/video_frames.csv", true);
  sess_avi_first_.store(kAviNone);
  sess_avi_last_.store(kAviNone);
  sess_avi_rows_.store(0);
  if (frames_fd_ < 0 || meta_fd_ < 0) {
    close_session("open_failed");
    return;
  }
  const char* fh =
      "frame_id,capture_us,pts_raw,ran_detect,detect_us,lat_us,detected,state,seq,age_ms,"
      "cand,yaw_deg,pitch_deg,vyaw_dps,vpitch_dps,dist_m,dist_valid,light_yaw_deg,"
      "light_pitch_deg,gx_px,gy_px,gr_px,has_box,"
      "mcu_state,mcu_flags,mcu_age_ms,mcu_rx_us,mcu_roll,mcu_pitch,mcu_yaw,mcu_gx,mcu_gy,mcu_gz\n";
  const char* mh = "rx_us,seq,state,flags,roll,pitch,yaw,gx,gy,gz\n";
  const char* vh2 = "avi_index,frame_id,capture_us,pts_raw\n";
  int err = 0;
  put_fd(frames_fd_, fh, static_cast<int>(strlen(fh)), &err);
  if (sess_video_fd_ >= 0) {
    put_fd(sess_video_fd_, vh2, static_cast<int>(strlen(vh2)), &err);
  }
  if (mcu_fd_ >= 0) {
    put_fd(mcu_fd_, mh, static_cast<int>(strlen(mh)), &err);
  }

  char meta[512];
  int mn = snprintf(meta, sizeof(meta),
                    "# fly_LicheeRV data session\n"
                    "session=%lu\nwall=%s\nuptime_s=%lu\ntrigger_mono_us=%llu\n"
                    "mode=%s\npreroll_ms=%d\npostroll_ms=%d\nmax_seconds=%d\n"
                    "mcu_stream=%d\n",
                    session_index_, wall_text().c_str(), uptime_s(),
                    static_cast<unsigned long long>(trigger_us),
                    cfg_.trigger_mode ? "trigger" : "continuous", cfg_.preroll_ms,
                    cfg_.postroll_ms, cfg_.max_seconds, cfg_.mcu_stream ? 1 : 0);
  if (mn > 0) {
    put_fd(meta_fd_, meta, mn, &err);
  }
  for (size_t i = 0; i < meta_.size(); ++i) {
    std::string line = meta_[i];
    if (line.empty() || line[line.size() - 1] != '\n') {
      line.push_back('\n');
    }
    put_fd(meta_fd_, line.c_str(), static_cast<int>(line.size()), &err);
  }

  session_start_us_ = monotonic_us();   // 用「本地开始写」计时，不用生产者给的时间戳：
                                        // 上游时间戳一旦是旧值/换过基准，max_seconds 会立刻误触发
  if (cfg_.trigger_mode) {
    // 预录窗口只保留最近 preroll_ms：环形缓冲本来能装更多（约 11 秒），
    // 但配置说 3 秒就只写 3 秒，避免文件里塞一堆用户没要的旧数据。
    const MonoUs pre = static_cast<MonoUs>(cfg_.preroll_ms) * 1000ULL;
    preroll_floor_us_ = trigger_us > pre ? trigger_us - pre : 0;
  } else {
    preroll_floor_us_ = 0;
  }
  sess_rows_frames_ = 0;
  sess_rows_mcu_ = 0;
  sess_dropped_ = dropped_.load();   // 本会话的丢行基线（空闲期丢最旧不算）
  session_open_ = true;
  land_seen_ = false;   // 新会话：重新等这次的落地边沿
  session_open_flag_.store(1);
  sessions_.fetch_add(1);
  last_session_event_us_.store(monotonic_us());
  land_us_.store(0);

  if (gt::diag(gt::kDiagLog)) {
    fprintf(stdout, "LOG_SESSION_OPEN dir=%s trigger_us=%llu\n", session_dir_.c_str(),
            static_cast<unsigned long long>(trigger_us));
    fflush(stdout);
  }
}

void FrameLog::close_session(const char* reason) {
  if (!session_open_ && frames_fd_ < 0 && mcu_fd_ < 0 && meta_fd_ < 0) {
    return;
  }
  const unsigned long dropped_now = dropped_.load() - sess_dropped_;
  int err = 0;
  // 先把数据文件收尾（fdatasync + close），最后才生成并写 meta。
  // 顺序反过来的话，最后一两帧会落在「meta 已生成、fd 还开着」的窗口里：
  // 行写进了 CSV，但计数没进 meta（实测差 1~2 行）。
  if (frames_fd_ >= 0) {
    ::fdatasync(frames_fd_);
  }
  if (mcu_fd_ >= 0) {
    ::fdatasync(mcu_fd_);
  }
  if (sess_video_fd_ >= 0) {
    ::fdatasync(sess_video_fd_);
  }
  if (frames_fd_ >= 0) {
    ::close(frames_fd_);
    frames_fd_ = -1;
  }
  if (mcu_fd_ >= 0) {
    ::close(mcu_fd_);
    mcu_fd_ = -1;
  }
  if (sess_video_fd_ >= 0) {
    ::close(sess_video_fd_);
    sess_video_fd_ = -1;
  }
  // 数据文件都已关闭，此刻的计数才是最终值 → 现在生成 meta
  char meta[512];
  const unsigned long avi_first = sess_avi_first_.load();
  const unsigned long avi_last = sess_avi_last_.load();
  const unsigned long avi_rows = sess_avi_rows_.load();
  const int mn = snprintf(meta, sizeof(meta),
                          "close_reason=%s\nclose_mono_us=%llu\nland_mono_us=%llu\n"
                          "rows_frames=%lu\nrows_mcu=%lu\ndropped=%lu\n"
                          "video=%s\nvideo_frames=%lu\nvideo_first_avi=%ld\nvideo_last_avi=%ld\n",
                          reason, static_cast<unsigned long long>(monotonic_us()),
                          static_cast<unsigned long long>(land_us_.load()), sess_rows_frames_,
                          sess_rows_mcu_, dropped_now, avi_rows > 0 ? "video.avi" : "(none)",
                          avi_rows, avi_first == kAviNone ? -1L : static_cast<long>(avi_first),
                          avi_last == kAviNone ? -1L : static_cast<long>(avi_last));
  if (meta_fd_ >= 0 && mn > 0) {
    put_fd(meta_fd_, meta, mn, &err);
  }
  if (meta_fd_ >= 0) {
    ::fdatasync(meta_fd_);
  }
  if (meta_fd_ >= 0) {
    ::close(meta_fd_);
    meta_fd_ = -1;
  }
  session_open_ = false;
  session_open_flag_.store(0);
  last_session_event_us_.store(monotonic_us());

  if (gt::diag(gt::kDiagLog)) {
    fprintf(stdout, "LOG_SESSION_CLOSE dir=%s reason=%s frames=%lu mcu=%lu dropped=%lu\n",
            session_dir_.c_str(), reason, sess_rows_frames_, sess_rows_mcu_, dropped_now);
    fflush(stdout);
  }
}

void FrameLog::poll_lifecycle() {
  const unsigned long seq = trigger_seq_.load();
  if (cfg_.trigger_mode) {
    if (seq != handled_trigger_) {
      handled_trigger_ = seq;
      if (session_open_) {
        // 出现了新的起飞边沿：先关掉上一段（可能是误触发），再开新的一段
        close_session("retriggered");
      }
      open_session(static_cast<MonoUs>(trigger_us_.load()));
      return;
    }
    if (session_open_) {
      const MonoUs now = monotonic_us();
      // 落地边沿用「写线程第一次看到它」的本地时刻计时，而不是生产者的时间戳：
      // 这样 postroll 不受两个时钟/基准不一致影响（协议本身也没有时间戳）
      if (!land_seen_ && land_us_.load() != 0) {
        land_seen_ = true;
        land_seen_us_ = now;
      }
      if (land_seen_ && now >= land_seen_us_ + static_cast<MonoUs>(cfg_.postroll_ms) * 1000ULL) {
        close_session("landed");
      } else if (now >= session_start_us_ + static_cast<MonoUs>(cfg_.max_seconds) * 1000000ULL) {
        close_session("max_seconds");
      }
    }
  } else if (!session_open_) {
    open_session(monotonic_us());   // 连续模式：没有会话就立刻开一段
  } else if (monotonic_us() >=
             session_start_us_ + static_cast<MonoUs>(cfg_.max_seconds) * 1000000ULL) {
    // 连续模式也要按 max_seconds 切段，否则一个文件会一直长到进程退出
    //（实测：12s 的配置跑 41s 只出一个 85MB 的文件，就是因为漏了这里）
    close_session("max_seconds");
  }
}

void FrameLog::sync_all() {
  if (session_open_) {
    if (frames_fd_ >= 0) {
      ::fdatasync(frames_fd_);
    }
    if (mcu_fd_ >= 0) {
      ::fdatasync(mcu_fd_);
    }
    if (meta_fd_ >= 0) {
      ::fdatasync(meta_fd_);
    }
  }
  // 只在锁内取 fd，真正的 fdatasync 放到锁外：否则相机线程的 1Hz 健康行
  // 会卡在 SD 卡的 fdatasync 上，跟着掉帧。
  int health = -1;
  int video = -1;
  {
    std::lock_guard<std::mutex> g(aux_mu_);
    health = health_fd_;
    video = video_fd_;
  }
  if (health >= 0) {
    ::fdatasync(health);
  }
  if (video >= 0) {
    ::fdatasync(video);
  }
}

void FrameLog::thread_main() {
  std::vector<Row> batch;
  batch.reserve(kBatch);
  double last_sync = mono_ms();

  while (!quit_.load()) {
    poll_lifecycle();

    batch.clear();
    {
      std::unique_lock<std::mutex> lk(mu_);
      // 空闲（还没触发）时环形缓冲是「故意不抽干」的，count_ 一直 > 0；
      // 如果只在 count_==0 时才睡，写线程会 100% 空转 —— 单核板上实测直接把
      // 采样耗时从 5ms 拖到 10ms、帧率从 80 掉到 52。所以空闲也必须睡。
      if (!session_open_ || count_ == 0) {
        cv_.wait_for(lk, std::chrono::milliseconds(kTickMs));
      }
      if (session_open_) {
        while (static_cast<int>(batch.size()) < kBatch && count_ > 0) {
          const Row& r = ring_[static_cast<size_t>(tail_)];
          // ts==0 的行（理论上没有）不参与裁剪；其余按预录窗口下界过滤
          if (preroll_floor_us_ == 0 || r.ts_us == 0 || r.ts_us >= preroll_floor_us_) {
            batch.push_back(r);
          }
          tail_ = (tail_ + 1) % cap_;
          --count_;
        }
      }
    }
    if (!batch.empty()) {
      write_rows(batch);
    }

    const double now_ms = mono_ms();
    if (now_ms - last_sync >= static_cast<double>(kSyncMs)) {
      last_sync = now_ms;
      sync_all();
    }
  }

  // 退出前把当前会话收尾（把缓冲里剩下的行也写掉）
  std::vector<Row> tail_rows;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_open_) {
      while (count_ > 0) {
        const Row& r = ring_[static_cast<size_t>(tail_)];
        if (preroll_floor_us_ == 0 || r.ts_us == 0 || r.ts_us >= preroll_floor_us_) {
          tail_rows.push_back(r);
        }
        tail_ = (tail_ + 1) % cap_;
        --count_;
      }
    }
  }
  if (!tail_rows.empty()) {
    write_rows(tail_rows);
  }
  if (session_open_) {
    close_session("shutdown");
  }
}

bool FrameLog::session_video_dir(std::string* out) const {
  if (!running_.load() || session_open_flag_.load() == 0) {
    return false;
  }
  std::lock_guard<std::mutex> g(aux_mu_);
  if (session_dir_.empty() || session_open_flag_.load() == 0) {
    return false;
  }
  if (out != nullptr) {
    *out = session_dir_;
  }
  return true;
}

LogStats FrameLog::stats() const {
  LogStats s;
  s.sessions = sessions_.load();
  s.rows_frames = rows_frames_.load();
  s.rows_mcu = rows_mcu_.load();
  s.rows_video = rows_video_.load();
  s.dropped = dropped_.load();
  s.triggers = triggers_.load();
  s.open = session_open_flag_.load() ? 1 : 0;
  const uint64_t last = last_session_event_us_.load();
  s.last_session_ms =
      last == 0 ? -1 : static_cast<int>((monotonic_us() - last) / 1000ULL);
  return s;
}

}  // namespace gt
