// v2 二进制帧的端到端自检：用 pty 冒充串口，跑生产代码 src/uart_out.cpp（cfg.ascii=false）
//
// 与 tests/uart_protocol_harness.cpp 的关系：
//   - 那个文件覆盖 ASCII v1（默认格式），37 项检查，别动它；
//   - 本文件只覆盖 uart_format=binary 这条新路径：抓真实字节，检查 A5 5A 帧头、28 字节定长、
//     CRC16 是否正确、连续帧的 SEQ 是否严格 +1 回绕、detected==0 时角度是否清零且 dist=-1.0，
//     以及确认二进制模式下**不会**混入 ASCII 字节。
//   - 帧内容本身（字节布局/CRC 值）已由 tests/uart_frame_test.cpp 在主机上逐字节对过文档示例，
//     这里验的是「串口线程真的按这个格式发、并且按帧序号递增」。
//
// 编译（Linux / WSL，需要 pty）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/uart_binary_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp -o /tmp/uart_bin_harness -pthread
//   然后 /tmp/uart_bin_harness
//
// 板端（交叉编译后 scp 上去跑）：
//   riscv64-unknown-linux-musl-g++ -std=c++11 -O2 -I src tests/uart_binary_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp -o /tmp/uart_bin_harness -static-libstdc++ -static-libgcc -pthread
#define _XOPEN_SOURCE 600

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "uart_frame.h"
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

// 读最多 ms 毫秒，把字节追加到 buf
void read_into(int fd, std::string* buf, int ms) {
  int elapsed = 0;
  while (elapsed < ms) {
    pollfd p{fd, POLLIN, 0};
    const int wait = (ms - elapsed) < 20 ? (ms - elapsed) : 20;
    const int rc = poll(&p, 1, wait);
    elapsed += wait;
    if (rc <= 0) {
      continue;
    }
    char chunk[1024];
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n > 0) {
      buf->append(chunk, static_cast<size_t>(n));
    }
  }
}

// 从字节流里切出完整 v2 帧；remaining 保留不完整的尾巴
void take_v2_frames(std::string* buf, std::vector<std::string>* out) {
  for (;;) {
    size_t start = 0;
    while (start + 1 < buf->size() &&
           !(static_cast<unsigned char>((*buf)[start]) == gt::kV2Head0 &&
             static_cast<unsigned char>((*buf)[start + 1]) == gt::kV2Head1)) {
      ++start;
    }
    if (start + 1 >= buf->size()) {
      buf->clear();
      return;
    }
    if (buf->size() - start < static_cast<size_t>(gt::kV2FrameLen)) {
      buf->erase(0, start);   // 保留可能的帧头，等更多字节
      return;
    }
    out->push_back(buf->substr(start, gt::kV2FrameLen));
    buf->erase(0, start + gt::kV2FrameLen);
  }
}

float get_f32(const std::string& f, int off) {
  float v = 0.0f;
  memcpy(&v, f.data() + off, sizeof(v));
  return v;
}

bool near(float a, float b) {
  const float d = a - b;
  return (d < 0.001f) && (d > -0.001f);
}

gt::TargetOut make_out(bool detected, gt::TargetState st, float yaw, float pitch, float vyaw,
                       float vpitch, float dist, bool dist_valid) {
  gt::TargetOut t;
  t.detected = detected;
  t.state = st;
  t.yaw_deg = yaw;
  t.pitch_deg = pitch;
  t.v_yaw_dps = vyaw;
  t.v_pitch_dps = vpitch;
  t.distance_m = dist;
  t.distance_valid = dist_valid;
  return t;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("=== UART v2 二进制帧端到端自检（pty 真实字节，跑生产代码 uart_out.cpp）===\n");

  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    printf("  FAIL  posix_openpt/grantpt/unlockpt\n");
    return 1;
  }
  const char* slave = ptsname(master);
  if (slave == nullptr) {
    printf("  FAIL  ptsname\n");
    return 1;
  }

  gt::UartOut uart;
  gt::UartCfg cfg;
  cfg.enable = true;
  cfg.device = slave;
  cfg.baud = 115200;
  cfg.rate_hz = 50;
  cfg.ascii = false;   // ← 本测试只覆盖二进制 v2 路径
  check(uart.start(cfg), "UartOut::start() 以 binary 模式打开 pty");

  std::string buf;
  std::vector<std::string> frames;

  // ---- 1. 有目标：detected=1，角度/距离按原值写入，字节序正确 ----
  uart.publish(make_out(true, gt::TargetState::kConfirmed, 12.34f, 5.67f, 45.2f, -12.3f, 3.456f,
                        true));
  read_into(master, &buf, 150);
  take_v2_frames(&buf, &frames);
  check(!frames.empty(), "有目标帧在 150ms 内到达", "frames=" + std::to_string(frames.size()));
  if (frames.empty()) {
    printf("  FAIL  没收到任何字节，后续检查无意义\n");
    uart.stop();
    return 1;
  }

  const std::string& f0 = frames.front();
  check(f0.size() == 28, "帧长 = 28 字节", "len=" + std::to_string(f0.size()));
  check(static_cast<unsigned char>(f0[0]) == 0xA5 && static_cast<unsigned char>(f0[1]) == 0x5A,
        "帧头 = A5 5A");
  check(static_cast<unsigned char>(f0[2]) == 0x15, "LEN = 0x15");
  check(static_cast<unsigned char>(f0[4]) == 0x01, "CMD = 0x01");
  const uint16_t crc_calc = gt::crc16_ccitt_false(
      reinterpret_cast<const uint8_t*>(f0.data()) + 2, 3 + gt::kV2Len);
  const uint16_t crc_wire = static_cast<uint16_t>(
      static_cast<unsigned char>(f0[26]) | (static_cast<unsigned char>(f0[27]) << 8));
  check(crc_calc == crc_wire, "CRC16 与线上字节一致");
  check(static_cast<unsigned char>(f0[5]) == 1, "detected = 1");
  check(near(get_f32(f0, 6), 12.34f), "yaw = 12.34");
  check(near(get_f32(f0, 10), 5.67f), "pitch = 5.67");
  check(near(get_f32(f0, 14), 45.2f), "vyaw = 45.20");
  check(near(get_f32(f0, 18), -12.3f), "vpitch = -12.30");
  check(near(get_f32(f0, 22), 3.456f), "dist = 3.456");
  check(f0.find('$') == std::string::npos && f0.find('G') == std::string::npos,
        "二进制模式下不混入 ASCII 字节");

  // ---- 2. 连续帧 SEQ 严格 +1 回绕 ----
  frames.clear();
  buf.clear();
  read_into(master, &buf, 300);   // 300ms @50Hz ≈ 15 帧
  take_v2_frames(&buf, &frames);
  check(frames.size() >= 10, "空闲时仍按 rate_hz 持续发（心跳）",
        "300ms 收到 " + std::to_string(frames.size()) + " 帧");
  bool seq_ok = frames.size() >= 2;
  for (size_t i = 1; i < frames.size(); ++i) {
    const unsigned prev = static_cast<unsigned char>(frames[i - 1][3]);
    const unsigned cur = static_cast<unsigned char>(frames[i][3]);
    if (cur != ((prev + 1) & 0xFF)) {
      seq_ok = false;
      printf("        SEQ 不连续：%u → %u\n", prev, cur);
      break;
    }
  }
  check(seq_ok, "SEQ 每帧 +1（0..255 回绕）");

  // ---- 3. 无目标：角度清零、dist=-1.0 ----
  frames.clear();
  buf.clear();
  uart.publish(make_out(false, gt::TargetState::kLost, 12.34f, 5.67f, 45.2f, -12.3f, 3.456f, true));
  read_into(master, &buf, 150);
  take_v2_frames(&buf, &frames);
  check(!frames.empty(), "无目标帧到达");
  if (!frames.empty()) {
    const std::string& f1 = frames.back();
    check(static_cast<unsigned char>(f1[5]) == 0, "detected = 0");
    check(near(get_f32(f1, 6), 0.0f) && near(get_f32(f1, 10), 0.0f), "角度被清零");
    check(near(get_f32(f1, 22), -1.0f), "dist = -1.0（无效）");
    const uint16_t c2 = gt::crc16_ccitt_false(
        reinterpret_cast<const uint8_t*>(f1.data()) + 2, 3 + gt::kV2Len);
    const uint16_t w2 = static_cast<uint16_t>(
        static_cast<unsigned char>(f1[26]) | (static_cast<unsigned char>(f1[27]) << 8));
    check(c2 == w2, "无目标帧 CRC 正确");
  }

  // ---- 4. 对端断开不崩溃 ----
  ::close(master);
  usleep(150 * 1000);
  uart.publish(make_out(true, gt::TargetState::kConfirmed, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, true));
  usleep(150 * 1000);
  check(uart.running(), "对端断开后发送线程仍在运行");

  uart.stop();
  check(!uart.running(), "stop() 后线程已停止");

  printf("\n=== 结果：%d 通过, %d 失败 ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
