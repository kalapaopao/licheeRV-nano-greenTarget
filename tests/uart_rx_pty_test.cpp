// 视觉侧「收电控帧」的端到端自检：pty 冒充串口，跑生产代码 src/uart_out.cpp
//
// 验的是：跑 licheeRV 程序时，电控发来的 CMD=0x02 帧真的被收下、解析正确、并能打印到终端。
//   - 电控帧被解析成 McuState（角度/角速度/状态/标志都对）
//   - 坏 CRC 帧计入 crc_errors 且不污染上一帧数据
//   - 回环收到自己的 0x01 帧只计入 other，不当成电控数据
//   - 接收打开后发送节奏不受影响（仍按 rate_hz 发 28 字节帧）
//
// 编译（Linux / WSL）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/uart_rx_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp -o /tmp/uart_rx_harness -pthread
//   然后 /tmp/uart_rx_harness
//
// 板端：
//   riscv64-unknown-linux-musl-g++ -std=c++11 -O2 -I src tests/uart_rx_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp -o /tmp/uart_rx_harness -static-libstdc++ -static-libgcc -pthread
#define _XOPEN_SOURCE 600

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
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

bool near(float a, float b) {
  const float d = a - b;
  return (d < 1e-3f) && (d > -1e-3f);
}

void write_all_fd(int fd, const uint8_t* p, int n) {
  int off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, p + off, static_cast<size_t>(n - off));
    if (w <= 0) return;
    off += static_cast<int>(w);
  }
}

// 从 master 读最多 ms 毫秒，返回读到的字节数
int drain_master(int fd, std::string* out, int ms) {
  int elapsed = 0;
  int total = 0;
  while (elapsed < ms) {
    pollfd p{fd, POLLIN, 0};
    const int wait = (ms - elapsed) < 20 ? (ms - elapsed) : 20;
    const int rc = poll(&p, 1, wait);
    elapsed += wait;
    if (rc <= 0) continue;
    char buf[1024];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (out != nullptr) out->append(buf, static_cast<size_t>(n));
      total += static_cast<int>(n);
    }
  }
  return total;
}

// 电控文档 §6.3（SEQ=7 STATE=1 FLAGS=0x07 roll=1.0 pitch=-2.0 yaw=3.5 gx=4.0 gy=-5.25 gz=6.75）
const uint8_t kMcuA[33] = {0xA5, 0x5A, 0x1A, 0x07, 0x02, 0x01, 0x07, 0x00, 0x00, 0x80, 0x3F,
                           0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x60, 0x40, 0x00, 0x00, 0x80,
                           0x40, 0x00, 0x00, 0xA8, 0xC0, 0x00, 0x00, 0xD8, 0x40, 0x73, 0x88};
// 电控文档 §6.4（SEQ=0 STATE=0 FLAGS=0 全零心跳）
const uint8_t kMcuIdle[33] = {0xA5, 0x5A, 0x1A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x38};

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("=== 视觉侧接收电控帧（CMD=0x02）端到端自检 ===\n");

  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    printf("  FAIL  posix_openpt\n");
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
  cfg.ascii = false;      // 发 v2 二进制帧（和板端实际配置一致）
  cfg.rx_print = true;    // 收到电控帧就打印
  cfg.rx_print_hz = 20;
  check(uart.start(cfg), "UartOut::start() 成功（发送 + 接收都开）");
  check(!uart.has_mcu(), "启动时还没有电控数据");

  // 发送方向同时也在跑（和真实程序一样：一边发目标帧、一边收电控帧）
  gt::TargetOut tx_target;
  tx_target.detected = false;
  uart.publish(tx_target);

  // ---- 1. 收一帧电控数据 ----
  write_all_fd(master, kMcuA, 33);
  usleep(120 * 1000);
  check(uart.has_mcu(), "收到电控帧后 has_mcu() == true");
  gt::McuState m = uart.last_mcu();
  check(m.seq == 7 && m.state == gt::kMcuStateFlight, "SEQ / STATE 正确");
  check(m.flags == 0x07 && m.imu_valid(), "FLAGS / IMU_VALID 正确");
  check(near(m.roll, 1.0f) && near(m.pitch, -2.0f) && near(m.yaw, 3.5f), "姿态角正确",
        "roll=1.0 pitch=-2.0 yaw=3.5");
  check(near(m.gx, 4.0f) && near(m.gy, -5.25f) && near(m.gz, 6.75f), "角速度正确");
  const int age = uart.mcu_age_ms();
  check(age >= 0 && age < 200, "数据年龄合理", "age=" + std::to_string(age) + "ms");
  printf("        （上面那一行 [MCU] ... 就是程序打到终端的实际输出）\n");

  // ---- 2. 心跳帧（STATE=0、FLAGS=0）也要能收 ----
  write_all_fd(master, kMcuIdle, 33);
  usleep(120 * 1000);
  m = uart.last_mcu();
  check(m.state == 0 && m.flags == 0, "心跳帧覆盖为 STATIC / flags=0");
  check(uart.rx_frames() >= 2, "rx_frames 累计", "rx_frames=" + std::to_string(uart.rx_frames()));

  // ---- 3. 坏 CRC：计数 +1，但不污染上一帧 ----
  uint8_t broken[33];
  memcpy(broken, kMcuA, 33);
  broken[20] ^= 0xFF;
  const unsigned long crc_before = uart.rx_crc_errors();
  write_all_fd(master, broken, 33);
  usleep(120 * 1000);
  check(uart.rx_crc_errors() > crc_before, "坏 CRC 被计入 rx_crc_errors");
  m = uart.last_mcu();
  check(m.state == 0 && m.flags == 0, "坏帧没有覆盖上一帧数据");

  // ---- 4. 回环收到自己的 0x01 帧：只计 other ----
  const gt::V2Data d = gt::v2_make_data(true, 1.0f, 2.0f, 3.0f, 4.0f, true, 5.0f);
  uint8_t own[28];
  gt::build_v2_frame(d, 1, own, sizeof(own));
  const unsigned long other_before = uart.rx_other_frames();
  write_all_fd(master, own, 28);
  usleep(120 * 1000);
  check(uart.rx_other_frames() > other_before, "0x01 帧计入 rx_other_frames");
  check(!near(uart.last_mcu().yaw, -1.0f), "0x01 帧没有被当成电控数据");

  // ---- 5. 发送方向没被接收拖坏 ----
  printf("        sent_frames=%lu send_errors=%lu\n", uart.sent_frames(), uart.send_errors());
  std::string tx;
  drain_master(master, &tx, 300);
  int frames = 0;
  for (size_t i = 0; i + 28 <= tx.size();) {
    if (static_cast<unsigned char>(tx[i]) == 0xA5 &&
        static_cast<unsigned char>(tx[i + 1]) == 0x5A) {
      ++frames;
      i += 28;
    } else {
      ++i;
    }
  }
  check(frames >= 10, "300ms 内仍在按 50Hz 发帧",
        "收到 " + std::to_string(frames) + " 帧 / 共 " + std::to_string(tx.size()) + " 字节");

  uart.stop();
  check(!uart.running(), "stop() 后线程已停止");
  ::close(master);

  printf("\n=== 结果：%d 通过, %d 失败 ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
