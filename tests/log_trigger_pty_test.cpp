// 「串口线程 → 记录器」端到端自检：pty 冒充串口，跑生产代码 uart_out.cpp + frame_log.cpp
//
// 验的是接线而不是算法（算法在 tests/frame_log_test.cpp 里单测）：
//   1. UartOut 收到的真 0x02 帧能触发 FrameLog 建会话（sink 挂上了、时钟用对的）；
//   2. tx_enable=false 时一个字节都不发（纯采集模式），但照样能收；
//   3. STATE=1 → 会话开；STATE=0 → postroll 后会话关；
//   4. mcu.csv 里能看到那几帧（含 STATE 值）。
//
// 编译（WSL / Linux 主机）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/log_trigger_pty_test.cpp
//       src/uart_out.cpp src/uart_frame.cpp src/frame_log.cpp -o /tmp/log_trigger_pty -pthread
#define _XOPEN_SOURCE 600

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include "frame_log.h"
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

void write_all_fd(int fd, const unsigned char* p, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, p + off, n - off);
    if (w > 0) {
      off += static_cast<size_t>(w);
      continue;
    }
    usleep(1000);
  }
}

int read_some(int fd, int ms) {
  int total = 0;
  int elapsed = 0;
  while (elapsed < ms) {
    pollfd p{fd, POLLIN, 0};
    const int rc = poll(&p, 1, 10);
    elapsed += 10;
    if (rc <= 0) continue;
    char buf[1024];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) total += static_cast<int>(n);
  }
  return total;
}

// 电控文档 §6.3（SEQ=7 STATE=1 FLIGHT FLAGS=0x07）
const unsigned char kFlight[33] = {0xA5, 0x5A, 0x1A, 0x07, 0x02, 0x01, 0x07, 0x00, 0x00, 0x80, 0x3F,
                                   0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x60, 0x40, 0x00, 0x00, 0x80,
                                   0x40, 0x00, 0x00, 0xA8, 0xC0, 0x00, 0x00, 0xD8, 0x40, 0x73, 0x88};
// 电控文档 §6.4（SEQ=0 STATE=0 STATIC FLAGS=0）
const unsigned char kIdle[33] = {0xA5, 0x5A, 0x1A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x38};

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

void sink(const gt::McuState& m, gt::MonoUs rx_us, void* user) {
  static_cast<gt::FrameLog*>(user)->on_mcu(m, rx_us);
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("=== 串口 → 记录器 触发链路自检（pty）===\n");

  char tmpl[] = "/tmp/logpty_XXXXXX";
  char* base = mkdtemp(tmpl);
  if (base == nullptr) {
    printf("  FAIL  mkdtemp\n");
    return 1;
  }

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

  gt::FrameLog log;
  gt::LogCfg lc;
  lc.enable = true;
  lc.trigger_mode = true;
  lc.mcu_stream = true;   // 本测试要验 mcu.csv
  lc.dir = std::string(base) + "/rec";
  lc.preroll_ms = 3000;
  lc.postroll_ms = 100;
  lc.max_seconds = 5;
  check(log.start(lc), "FrameLog start()");

  gt::UartOut uart;
  gt::UartCfg uc;
  uc.enable = true;
  uc.tx_enable = false;   // 纯采集：不发任何字节
  uc.device = slave;
  uc.baud = 115200;
  uc.rate_hz = 50;
  uc.ascii = false;
  uc.rx_print = false;
  uart.set_mcu_sink(&sink, &log);   // 必须在 start() 之前
  check(uart.start(uc), "UartOut start()（tx 关闭、rx 打开）");
  usleep(100 * 1000);

  // 1. 静止心跳：不该触发
  write_all_fd(master, kIdle, sizeof(kIdle));
  usleep(150 * 1000);
  check(uart.has_mcu(), "收到了 STATE=STATIC 心跳");
  check(log.stats().sessions == 0, "STATIC 不触发会话");

  // 2. 起飞边沿
  write_all_fd(master, kFlight, sizeof(kFlight));
  usleep(200 * 1000);
  check(log.stats().triggers >= 1, "STATE=FLIGHT 产生了起飞边沿");
  check(log.stats().sessions == 1, "起飞边沿让记录器建了会话", "sessions=" + std::to_string(log.stats().sessions));
  check(log.stats().open == 1, "会话正在写");

  // 3. tx 关闭时线上不该有任何字节
  const int leaked = read_some(master, 150);
  check(leaked == 0, "tx_enable=false 时串口上一个字节都没发",
        "read=" + std::to_string(leaked));

  // 4. 落地边沿 → postroll 后收尾
  write_all_fd(master, kIdle, sizeof(kIdle));
  usleep(400 * 1000);
  check(log.stats().open == 0, "落地 + postroll 后会话关闭");

  uart.stop();
  const gt::LogStats st = log.stats();
  log.stop();
  check(st.rows_mcu >= 3, "mcu.csv 至少落盘 3 行", "rows=" + std::to_string(st.rows_mcu));

  // 5. 文件内容：STATE 1 那行必须在
  std::string listing;
  {
    std::string cmd = std::string("ls ") + base + "/rec | grep sess_ | head -1";
    FILE* p = popen(cmd.c_str(), "r");
    char buf[256] = {0};
    if (p != nullptr) {
      if (fgets(buf, sizeof(buf), p) != nullptr) {
        std::string s(buf);
        while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) s.erase(s.size() - 1);
        listing = s;
      }
      pclose(p);
    }
  }
  check(!listing.empty(), "生成了会话目录", listing);
  const std::string mcu = slurp(std::string(base) + "/rec/" + listing + "/mcu.csv");
  check(mcu.find(",1,7,") != std::string::npos, "mcu.csv 里有 STATE=1 FLAGS=0x07 那行");
  const std::string meta = slurp(std::string(base) + "/rec/" + listing + "/meta.txt");
  check(meta.find("mode=trigger") != std::string::npos, "meta.txt 记录了 trigger 模式");

  printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
