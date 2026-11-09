// 串口协议端到端自检：用 pty 冒充真实串口，跑的是生产代码 src/uart_out.cpp
//
// 目的（给电控联调前的自证，不是单元测试的替代）：
//   1. 抓 Real UartOut 输出的**真实字节**，逐帧校验格式 / 字段数 / XOR 校验和；
//   2. 验证四种状态（启动帧 / confirmed / coasting / lost）下每个字段的取值含义；
//   3. 验证固定频率发送（没有新结果时仍按 rate_hz 重复发同一份快照）；
//   4. 验证对端断开（master 关闭）时模块不崩溃、errors 增加、主流程不受影响；
//   5. 验证「取帧→开始写」年龄统计确实在采样（诊断日志用）。
//
// 编译（主机）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/uart_protocol_harness.cpp src/uart_out.cpp
//       -o /tmp/uart_harness -pthread
// 运行：
//   /tmp/uart_harness
//
// 板端交叉编译（可选，用来证明 riscv64 产物字节一致）：
//   riscv64-unknown-linux-musl-g++ -std=c++11 -O2 -I src tests/uart_protocol_harness.cpp
//       src/uart_out.cpp -o /tmp/uart_harness -static-libstdc++ -static-libgcc -pthread
#define _XOPEN_SOURCE 600

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "frame_timing.h"
#include "uart_out.h"

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

struct Frame {
  std::string raw;                    // "$GT,...*CS\r\n"
  std::string body;                   // "$GT," 与 "*" 之间
  std::string cs;                     // 两位十六进制
  std::vector<std::string> field;     // body 按 ',' 切分
  bool cs_ok = false;
};

unsigned char xor_of(const std::string& s) {
  unsigned char c = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    c = static_cast<unsigned char>(c ^ static_cast<unsigned char>(s[i]));
  }
  return c;
}

// 从 buf 中切出完整帧（以 "\r\n" 结尾），返回切出的数量，剩余留在 buf
void take_frames(std::string* buf, std::vector<Frame>* out) {
  for (;;) {
    const size_t pos = buf->find("\r\n");
    if (pos == std::string::npos) {
      return;
    }
    const std::string raw = buf->substr(0, pos + 2);
    buf->erase(0, pos + 2);

    Frame f;
    f.raw = raw;
    const size_t star = raw.find('*');
    if (raw.compare(0, 4, "$GT,") == 0 && star != std::string::npos && star + 2 <= raw.size()) {
      f.body = raw.substr(4, star - 4);
      f.cs = raw.substr(star + 1, 2);
      f.cs_ok = (static_cast<unsigned char>(strtoul(f.cs.c_str(), nullptr, 16)) ==
                 static_cast<unsigned char>(xor_of(f.body)));
      size_t start = 0;
      for (;;) {
        const size_t comma = f.body.find(',', start);
        if (comma == std::string::npos) {
          f.field.push_back(f.body.substr(start));
          break;
        }
        f.field.push_back(f.body.substr(start, comma - start));
        start = comma + 1;
      }
    }
    out->push_back(f);
  }
}

// 读取 master 端最多 ms 毫秒，返回是否有数据
bool drain(int fd, std::string* buf, std::vector<Frame>* out, int ms) {
  bool got = false;
  const long long deadline = ms;
  long long elapsed = 0;
  while (elapsed < deadline) {
    pollfd p{fd, POLLIN, 0};
    const int rc = poll(&p, 1, static_cast<int>(deadline - elapsed < 20 ? deadline - elapsed : 20));
    elapsed += 20;
    if (rc <= 0) {
      continue;
    }
    char chunk[512];
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      continue;
    }
    got = true;
    buf->append(chunk, static_cast<size_t>(n));
    take_frames(buf, out);
  }
  return got;
}

const char* get(const Frame& f, size_t i) {
  return (i < f.field.size()) ? f.field[i].c_str() : "<missing>";
}

Frame find_state(const std::vector<Frame>& frames, const char* state) {
  for (size_t i = frames.size(); i > 0; --i) {
    if (frames[i - 1].field.size() == 9 && frames[i - 1].field[8] == state) {
      return frames[i - 1];
    }
  }
  return Frame();
}

void dump(const Frame& f) {
  printf("        raw : %s", f.raw.c_str());
  if (f.field.size() == 9) {
    printf("        body: det=%s yaw=%s pitch=%s vyaw=%s vpitch=%s dist=%s age=%s seq=%s state=%s\n",
           get(f, 0), get(f, 1), get(f, 2), get(f, 3), get(f, 4), get(f, 5), get(f, 6), get(f, 7),
           get(f, 8));
  }
  printf("        xor : %s%s\n", f.cs_ok ? "OK" : "BAD", f.cs.c_str());
}

gt::TargetOut make_target(gt::TargetState state, bool detected, float yaw, float pitch, float vyaw,
                          float vpitch, float dist, bool dist_valid, unsigned long seq,
                          float age_ms) {
  gt::TargetOut t;
  t.detected = detected;
  t.state = state;
  t.yaw_deg = yaw;
  t.pitch_deg = pitch;
  t.v_yaw_dps = vyaw;
  t.v_pitch_dps = vpitch;
  t.distance_m = dist;
  t.distance_valid = dist_valid;
  t.seq = seq;
  t.age_ms = age_ms;
  return t;
}

}  // namespace

int main() {
  signal(SIGHUP, SIG_IGN);   // 模拟对端断开时不要被 SIGHUP 打死
  setvbuf(stdout, nullptr, _IOLBF, 0);

  printf("=== UART 协议端到端自检（pty 真实字节，跑生产代码 uart_out.cpp）===\n");

  // ---- 建 pty 对 ----
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    printf("  FAIL  posix_openpt/grantpt/unlockpt\n");
    return 1;
  }
  const char* slave = ptsname(master);
  printf("  INFO  pty master=%d slave=%s\n", master, slave ? slave : "(null)");
  if (slave == nullptr) {
    return 1;
  }

  // ---- 启动生产代码里的 UartOut ----
  gt::UartOut uart;
  gt::UartCfg cfg;
  cfg.enable = true;
  cfg.device = slave;
  cfg.baud = 115200;
  cfg.rate_hz = 50;
  cfg.ascii = true;
  check(uart.start(cfg), "UartOut::start() 成功打开 pty 并启动发送线程");
  check(uart.running(), "发送线程 running() = true");

  std::string buf;
  std::vector<Frame> frames;

  // ---- 1. 启动帧（无时间戳）----
  gt::TargetOut boot = make_target(gt::TargetState::kLost, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                   false, 0, -1.0f);
  uart.publish(boot);                       // 旧重载：has_timing = false
  drain(master, &buf, &frames, 100);
  Frame boot_f = find_state(frames, "0");
  check(!frames.empty(), "启动帧到达对端（100ms 内）", "frames=" + std::to_string(frames.size()));
  if (frames.empty()) {
    printf("  FAIL  没有收到任何字节，后续检查无意义\n");
    return 1;
  }
  check(boot_f.cs_ok, "启动帧 XOR 校验和正确");
  check(boot_f.field.size() == 9, "启动帧字段数 = 9（det,yaw,pitch,vyaw,vpitch,dist,age,seq,state）");
  check(std::string(get(boot_f, 0)) == "0", "启动帧 det=0（无目标）", get(boot_f, 0));
  check(std::string(get(boot_f, 6)) == "-1", "启动帧 age=-1（从未检测到）", get(boot_f, 6));
  check(std::string(get(boot_f, 7)) == "0", "启动帧 seq=0", get(boot_f, 7));
  printf("  ---- 启动帧原始字节 ----\n");
  dump(boot_f);

  // ---- 2. confirmed：带采集时间戳，模拟「取帧后 8ms 才发送」----
  frames.clear();
  buf.clear();
  const gt::MonoUs now_us = gt::monotonic_us();
  gt::FrameTiming timing;
  timing.frame_id = 42;
  timing.vi_pts_raw = 123456789ULL;
  timing.capture_mono_us = now_us - 8000ULL;   // 8ms 前取到帧
  timing.detect_start_us = now_us - 7800ULL;
  timing.detect_end_us = now_us - 3000ULL;
  gt::TargetOut confirmed =
      make_target(gt::TargetState::kConfirmed, true, 12.34f, 5.67f, 45.2f, -12.3f, 3.456f, true,
                  1234UL, 0.0f);
  uart.publish(confirmed, timing);
  drain(master, &buf, &frames, 120);
  Frame conf = find_state(frames, "2");
  check(!conf.raw.empty(), "confirmed 帧到达（state=2）");
  check(conf.cs_ok, "confirmed 帧 XOR 校验和正确");
  check(conf.field.size() == 9, "confirmed 帧字段数 = 9");
  check(std::string(get(conf, 0)) == "1", "confirmed det=1", get(conf, 0));
  check(std::string(get(conf, 1)) == "12.34", "yaw 原样发出（2 位小数）", get(conf, 1));
  check(std::string(get(conf, 2)) == "5.67", "pitch 原样发出", get(conf, 2));
  check(std::string(get(conf, 3)) == "45.20", "vyaw 2 位小数", get(conf, 3));
  check(std::string(get(conf, 4)) == "-12.30", "vpitch 负值 2 位小数", get(conf, 4));
  check(std::string(get(conf, 5)) == "3.456", "dist 3 位小数", get(conf, 5));
  check(std::string(get(conf, 6)) == "0", "age=0（本帧刚检测）", get(conf, 6));
  check(std::string(get(conf, 7)) == "1234", "seq=1234", get(conf, 7));
  check(std::string(get(conf, 8)) == "2", "state=2", get(conf, 8));
  printf("  ---- confirmed 原始字节 ----\n");
  dump(conf);

  // ---- 3. coasting：det 仍为 1，距离无效 → 0.000，age 增长 ----
  frames.clear();
  buf.clear();
  gt::TargetOut coasting =
      make_target(gt::TargetState::kCoasting, true, 12.5f, 5.6f, 0.0f, 0.0f, 9.999f, false,
                  1240UL, 123.0f);
  uart.publish(coasting, timing);
  drain(master, &buf, &frames, 120);
  Frame coast = find_state(frames, "1");
  check(!coast.raw.empty(), "coasting 帧到达（state=1）");
  check(coast.cs_ok, "coasting 帧 XOR 校验和正确");
  check(std::string(get(coast, 0)) == "1", "coasting 仍报 det=1（预测继续输出）", get(coast, 0));
  check(std::string(get(coast, 5)) == "0.000", "distance_valid=false → dist=0.000", get(coast, 5));
  check(std::string(get(coast, 6)) == "123", "age=123（测量年龄原样发出）", get(coast, 6));
  printf("  ---- coasting 原始字节 ----\n");
  dump(coast);

  // ---- 4. lost：det=0、seq 冻结、age 继续变大 ----
  frames.clear();
  buf.clear();
  gt::TargetOut lost = make_target(gt::TargetState::kLost, false, 12.5f, 5.6f, 0.0f, 0.0f, 0.0f,
                                   false, 1240UL, 250.0f);
  uart.publish(lost, timing);
  drain(master, &buf, &frames, 120);
  Frame lost_f = find_state(frames, "0");
  check(!lost_f.raw.empty(), "lost 帧到达（state=0）");
  check(lost_f.cs_ok, "lost 帧 XOR 校验和正确");
  check(std::string(get(lost_f, 0)) == "0", "lost det=0", get(lost_f, 0));
  check(std::string(get(lost_f, 7)) == "1240", "lost 时 seq 冻结（同一序号重复发）", get(lost_f, 7));
  check(std::string(get(lost_f, 6)) == "250", "lost 时 age 继续增长", get(lost_f, 6));

  // ---- 5. 固定频率：没有新结果时按 rate_hz 重复发同一份快照 ----
  buf.clear();
  frames.clear();
  drain(master, &buf, &frames, 400);   // 400ms @50Hz ≈ 20 帧
  check(frames.size() >= 16 && frames.size() <= 24,
        "空闲时仍按 50Hz 重复发送", "400ms 收到 " + std::to_string(frames.size()) + " 帧");
  bool same_payload = true;
  for (size_t i = 1; i < frames.size(); ++i) {
    if (frames[i].body != frames[0].body) {
      same_payload = false;
    }
  }
  check(same_payload, "重复帧内容完全相同（只发最新快照，不刷新 age）");

  // ---- 6. 诊断统计：写开始年龄确实在采样 ----
  // 先清零（前面几个阶段用的是固定 capture，重复发送时年龄会一直变大，不适合做数值检查），
  // 再用「取帧后 8ms」的新鲜时间戳采一小段，检查量级。
  uart.take_write_start_age_stats();
  gt::FrameTiming fresh = timing;
  fresh.capture_mono_us = gt::monotonic_us() - 8000ULL;
  uart.publish(confirmed, fresh);
  drain(master, &buf, &frames, 40);
  gt::WriteStartAgeStats st = uart.take_write_start_age_stats();
  check(st.samples > 0, "写开始年龄样本数 > 0", "samples=" + std::to_string(st.samples));
  check(st.avg_ms >= 8.0 && st.avg_ms <= 60.0,
        "平均写开始年龄 ≈ 取帧后 8ms + 等待发送的一小段", "avg=" + std::to_string(st.avg_ms) + "ms");
  printf("  INFO  本窗口写开始年龄：last=%dms avg=%.2fms max=%dms samples=%lu\n", st.last_ms,
         st.avg_ms, st.max_ms, st.samples);

  // ---- 7. 对端断开：不崩溃、errors 增长、主流程不受影响 ----
  const unsigned long err_before = uart.send_errors();
  ::close(master);   // 模拟电控拔线
  usleep(150 * 1000);
  uart.publish(confirmed, timing);
  usleep(150 * 1000);
  check(uart.running(), "对端断开后发送线程仍在运行（不崩溃/不死锁）");
  check(uart.send_errors() > err_before, "对端断开被记录为 send_errors 增长",
        "before=" + std::to_string(err_before) + " after=" + std::to_string(uart.send_errors()));

  uart.stop();
  check(!uart.running(), "stop() 后线程已停止");

  printf("\n=== 结果：%d 通过, %d 失败 ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
