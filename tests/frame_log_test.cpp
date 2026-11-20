// FrameLog 主机单测（不需要板子、不需要串口）
//
// 验的是「触发 + 预录」这套逻辑本身：
//   1. 触发前不落盘，但环形缓冲一直攒着（预录）；
//   2. 收到 MCU STATE STATIC→FLIGHT 后，预录的旧行**也要**进文件（这是 T+ 方案的全部意义）；
//   3. FLIGHT→STOP 后再写 postroll 毫秒就收尾，meta.txt 里 close_reason=landed；
//   4. max_seconds 到点强制收尾（防止一直不落地把卡写满）；
//   5. 连续模式：开机即开一段，不依赖电控；
//   6. CSV 表头列数 == 数据列数（防止改字段时忘了改表头，导入图表时错列）。
//
// 编译（WSL / Linux 主机）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/frame_log_test.cpp src/frame_log.cpp
//       -o /tmp/frame_log_test -pthread && /tmp/frame_log_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "frame_log.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what, const std::string& detail = "") {
  if (ok) {
    ++g_pass;
    printf("  PASS  %s%s%s\n", what.c_str(), detail.empty() ? "" : "   ", detail.c_str());
  } else {
    ++g_fail;
    printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : "   ", detail.c_str());
  }
}

std::string slurp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) return std::string();
  std::string s;
  char buf[4096];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(s[i]);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

int columns(const std::string& line) {
  int n = 1;
  for (size_t i = 0; i < line.size(); ++i)
    if (line[i] == ',') ++n;
  return n;
}

gt::FrameTiming timing_of(uint32_t id) {
  gt::FrameTiming t;
  t.frame_id = id;
  t.capture_mono_us = 1000000ULL + static_cast<uint64_t>(id) * 12500ULL;
  t.vi_pts_raw = t.capture_mono_us;
  t.detect_start_us = t.capture_mono_us;
  t.detect_end_us = t.capture_mono_us + 5000;
  return t;
}

gt::TargetOut target_of(uint32_t id) {
  gt::TargetOut o;
  o.detected = true;
  o.state = gt::TargetState::kConfirmed;
  o.yaw_deg = 1.5f + static_cast<float>(id) * 0.01f;
  o.pitch_deg = -0.5f;
  o.v_yaw_dps = 12.0f;
  o.distance_m = 2.5f;
  o.distance_valid = true;
  o.seq = id;
  o.age_ms = 0.0f;
  o.candidates = 1;
  o.has_box = true;
  o.gx = 160.0f;
  o.gy = 90.0f;
  o.gr = 8.0f;
  return o;
}

void feed_frames(gt::FrameLog* log, uint32_t from, uint32_t to) {
  for (uint32_t i = from; i < to; ++i) {
    log->on_frame(timing_of(i), target_of(i), 1, 4, nullptr, -1, 0);
  }
}

gt::McuState mcu_state(uint8_t st, uint8_t seq) {
  gt::McuState m;
  m.state = st;
  m.flags = 0x07;
  m.seq = seq;
  m.roll = 1.0f;
  m.pitch = -2.0f;
  m.yaw = 3.5f;
  m.gx = 4.0f;
  m.gy = -5.25f;
  m.gz = 6.75f;
  return m;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("=== FrameLog 预录触发自检 ===\n");

  char tmpl[] = "/tmp/framelog_test_XXXXXX";
  char* base = mkdtemp(tmpl);
  if (base == nullptr) {
    printf("  FAIL  mkdtemp\n");
    return 1;
  }
  const std::string root(base);
  printf("  tmp %s\n", root.c_str());

  // ---- 1. enable=false：什么都不做 ----
  {
    gt::FrameLog log;
    gt::LogCfg c;
    c.enable = false;
    check(log.start(c), "enable=false 时 start() 返回 true");
    check(!log.running(), "enable=false 时 running() == false");
    log.on_frame(timing_of(1), target_of(1), 1, 4, nullptr, -1, 0);
    log.stop();   // 不应崩溃
    check(true, "enable=false 时 on_frame/stop 安全");
  }

  // ---- 2. 连续模式 ----
  {
    gt::FrameLog log;
    gt::LogCfg c;
    c.enable = true;
    c.trigger_mode = false;
    c.mcu_stream = true;      // 本测试要验 mcu.csv，显式打开流
    c.dir = root + "/cont";
    c.ring_rows = 512;
    check(log.start(c), "连续模式 start() 成功");
    usleep(120 * 1000);
    check(log.stats().open == 1, "连续模式开机即开一段");
    feed_frames(&log, 1, 11);
    for (int i = 0; i < 5; ++i) log.on_mcu(mcu_state(gt::kMcuStateFlight, static_cast<uint8_t>(i)), 2000 + i * 10000);
    usleep(200 * 1000);
    log.on_video(0, 7, 1007000, 1007000);
    usleep(50 * 1000);
    const gt::LogStats st = log.stats();
    check(st.rows_frames == 10, "连续模式落盘 10 行帧数据", "rows=" + std::to_string(st.rows_frames));
    check(st.rows_mcu == 5, "连续模式落盘 5 行电控数据", "rows=" + std::to_string(st.rows_mcu));
    log.stop();
  }

  // ---- 3. 触发模式：预录必须进文件 ----
  {
    gt::FrameLog log;
    gt::LogCfg c;
    c.enable = true;
    c.trigger_mode = true;
    c.mcu_stream = true;
    c.dir = root + "/trig";
    c.preroll_ms = 3000;
    c.postroll_ms = 100;
    c.max_seconds = 5;
    c.ring_rows = 512;
    check(log.start(c), "触发模式 start() 成功");

    // 触发前：1..50 帧 + 若干电控心跳（STATE=0）
    feed_frames(&log, 1, 51);
    for (int i = 0; i < 10; ++i) log.on_mcu(mcu_state(gt::kMcuStateStatic, static_cast<uint8_t>(i)), 1000 + i * 10000);
    usleep(150 * 1000);
    check(log.stats().sessions == 0, "触发前不建会话");
    check(log.stats().open == 0, "触发前不落盘");

    // 起飞边沿
    log.on_mcu(mcu_state(gt::kMcuStateFlight, 11), 2000000);
    usleep(150 * 1000);
    check(log.stats().sessions == 1, "STATIC→FLIGHT 建了 1 个会话");
    check(log.stats().open == 1, "触发后会话处于打开状态");

    // 飞行中继续记
    feed_frames(&log, 51, 61);
    log.on_mcu(mcu_state(gt::kMcuStateFlight, 12), 2100000);
    usleep(120 * 1000);

    // 落地
    log.on_mcu(mcu_state(gt::kMcuStateStop, 13), 2200000);
    usleep(300 * 1000);   // > postroll(100ms)，等写线程收尾
    check(log.stats().open == 0, "落地 + postroll 后会话关闭");
    const unsigned long rows_before_stop = log.stats().rows_frames;
    check(rows_before_stop >= 60, "预录 + 飞行中的帧行都落盘了",
          "rows=" + std::to_string(rows_before_stop));
    log.stop();

    // 校验文件内容
    char path[512];
    snprintf(path, sizeof(path), "%s/trig", root.c_str());
    std::string dirname;
    // 找会话目录（名字里带 sess_）
    {
      std::string cmd = std::string("ls ") + path + " | grep sess_ | head -1";
      FILE* p = popen(cmd.c_str(), "r");
      char buf[256] = {0};
      if (p != nullptr) {
        if (fgets(buf, sizeof(buf), p) != nullptr) {
          std::string s(buf);
          while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) s.erase(s.size() - 1);
          dirname = s;
        }
        pclose(p);
      }
    }
    check(!dirname.empty(), "生成了 sess_ 会话目录", dirname);

    const std::string sess = path + std::string("/") + dirname;
    const std::vector<std::string> fl = split_lines(slurp(sess + "/frames.csv"));
    const std::vector<std::string> ml = split_lines(slurp(sess + "/mcu.csv"));
    const std::string meta = slurp(sess + "/meta.txt");

    check(fl.size() >= 61, "frames.csv 行数 = 表头 + 预录50 + 飞行中10",
          "lines=" + std::to_string(fl.size()));
    check(!fl.empty() && fl[0].compare(0, 9, "frame_id,") == 0, "frames.csv 表头正确");
    // 第一行数据必须是 frame_id=1（预录的左边界，说明触发前那 50 行没丢）
    check(fl.size() > 1 && fl[1].compare(0, 2, "1,") == 0, "预录第一行是 frame_id=1",
          fl.size() > 1 ? fl[1].substr(0, 24) : "");
    check(!ml.empty() && columns(ml[0]) == 10, "mcu.csv 表头 10 列");
    if (ml.size() > 1) {
      check(columns(ml[1]) == columns(ml[0]), "mcu.csv 每行列数与表头一致");
    }
    check(columns(fl[0]) == columns(fl[1]), "frames.csv 列数与表头一致",
          "hdr=" + std::to_string(columns(fl[0])) + " row=" + std::to_string(columns(fl[1])));
    check(columns(fl[0]) == 33, "frames.csv 33 列（含电控 7 个字段）",
          "cols=" + std::to_string(columns(fl[0])));
    check(fl[0].find("mcu_roll,mcu_pitch,mcu_yaw,mcu_gx,mcu_gy,mcu_gz") != std::string::npos,
          "表头末尾是电控 6 个浮点");
    check(fl.size() > 1 && fl[1].find(",nan,nan,") != std::string::npos,
          "没有电控数据时 6 个浮点写 nan、列数仍恒定");
    check(meta.find("close_reason=landed") != std::string::npos, "meta.txt 记录了 landed 收尾原因");
    check(meta.find("trigger_mono_us=2000000") != std::string::npos, "meta.txt 记录了触发时刻");
  }

  // ---- 3b. 预录窗口按 preroll_ms 裁剪（不能把环形缓冲里全部 11 秒都写进去）----
  {
    gt::FrameLog log;
    gt::LogCfg c;
    c.enable = true;
    c.trigger_mode = true;
    c.dir = root + "/trim";
    c.preroll_ms = 200;
    c.postroll_ms = 50;
    c.max_seconds = 5;
    c.ring_rows = 512;
    check(log.start(c), "裁剪测试 start() 成功");
    const gt::MonoUs now_us = gt::monotonic_us();
    // 50 帧，跨度 625ms，全部在触发之前
    for (int i = 0; i < 50; ++i) {
      gt::FrameTiming t;
      t.frame_id = static_cast<uint32_t>(1 + i);
      t.capture_mono_us = now_us - 625000ULL + static_cast<gt::MonoUs>(i) * 12500ULL;
      t.detect_start_us = t.capture_mono_us;
      t.detect_end_us = t.capture_mono_us + 5000;
      log.on_frame(t, target_of(static_cast<uint32_t>(1 + i)), 1, 4, nullptr, -1, 0);
    }
    usleep(100 * 1000);
    log.on_mcu(mcu_state(gt::kMcuStateFlight, 1), now_us);   // 触发
    usleep(200 * 1000);
    log.on_mcu(mcu_state(gt::kMcuStateStop, 2), now_us + 1000);
    usleep(300 * 1000);
    log.stop();

    std::string sess;
    {
      std::string cmd = std::string("ls ") + root + "/trim | grep sess_ | head -1";
      FILE* p = popen(cmd.c_str(), "r");
      char buf[256] = {0};
      if (p != nullptr) {
        if (fgets(buf, sizeof(buf), p) != nullptr) {
          std::string s(buf);
          while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) s.erase(s.size() - 1);
          sess = s;
        }
        pclose(p);
      }
    }
    const std::vector<std::string> fl = split_lines(slurp(root + "/trim/" + sess + "/frames.csv"));
    // 200ms / 12.5ms ≈ 16 帧（允许 ±4 帧的时序抖动）
    const int rows = static_cast<int>(fl.size()) - 1;
    check(rows >= 12 && rows <= 20, "预录裁剪：200ms 窗口只剩约 16 帧",
          "rows=" + std::to_string(rows));
  }

  // ---- 4. max_seconds 强制收尾 ----
  {
    gt::FrameLog log;
    gt::LogCfg c;
    c.enable = true;
    c.trigger_mode = true;
    c.dir = root + "/cap";
    c.postroll_ms = 5000;   // 故意很长：不落地时只能靠 max_seconds 收尾
    c.max_seconds = 1;
    c.ring_rows = 512;
    check(log.start(c), "上限测试 start() 成功");
    log.on_mcu(mcu_state(gt::kMcuStateFlight, 1), 1000000);
    usleep(150 * 1000);
    check(log.stats().open == 1, "上限测试：起飞后会话已开");
    usleep(1200 * 1000);
    check(log.stats().open == 0, "max_seconds 到点强制收尾");
    log.stop();
  }

  // ---- 5. 空闲时写线程不许空转 ----
  // 板上实测过一次：预录期环形缓冲「故意不抽干」→ count_ 恒 >0 → 写线程只在
  // count_==0 时才睡 → 100% 空转，单核 CPU 直接把采样从 5ms 拖到 10ms、80fps 掉到 52fps。
  {
    struct rusage ru0 {};
    struct rusage ru1 {};
    getrusage(RUSAGE_SELF, &ru0);
    gt::FrameLog log;
    gt::LogCfg c;
    c.enable = true;
    c.trigger_mode = true;
    c.dir = root + "/idle";
    c.ring_rows = 512;
    log.start(c);
    feed_frames(&log, 1, 41);   // 填满预录窗口但一直不触发
    usleep(1000 * 1000);
    getrusage(RUSAGE_SELF, &ru1);
    const double cpu_s = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) +
                         (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6 +
                         (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
                         (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;
    char detail[64];
    snprintf(detail, sizeof(detail), "cpu=%.3fs", cpu_s);
    check(cpu_s < 0.30, "空闲 1 秒 CPU < 0.30s（写线程没有空转）", detail);
    log.stop();
  }

  // ---- 6. 没触发时不能产生任何文件（健康行/侧车都跟着会话走）----
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/idle", root.c_str());
    std::string cmd = std::string("ls ") + path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    std::string listing;
    if (p != nullptr) {
      char buf[256];
      while (fgets(buf, sizeof(buf), p) != nullptr) listing += buf;
      pclose(p);
    }
    check(listing.find("health") == std::string::npos,
          "待机时不生成 health_*.csv", "dir=" + std::string(path));
    check(listing.find("video_frames") == std::string::npos,
          "待机时不生成 video_frames_*.csv");
  }

  printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
