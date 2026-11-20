// 诊断层实现，见 diag.h
#include "diag.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>

namespace gt {
namespace {

unsigned g_mask = kDiagAll;          // 默认全开：不改配置时行为与之前一致
char g_state_dir[192] = {0};

// 崩溃处理器只能读这些普通变量（不支持原子性，但用来写"最后一刻大概在哪"足够）
volatile unsigned long g_frames = 0;
volatile unsigned long g_encoded = 0;
volatile int g_temp_c = -1;

const char* kHistoryName = "/run_history.log";
const off_t kRotateBytes = 256 * 1024;

unsigned long uptime_s() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<unsigned long>(ts.tv_sec);
}

// 手写整数转字符串：崩溃处理器里不能用 snprintf（不是异步信号安全的）
int put_ulong(char* p, int cap, unsigned long v) {
  char tmp[24];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  }
  while (v > 0 && n < 24) {
    tmp[n++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  }
  int out = 0;
  while (n > 0 && out < cap - 1) p[out++] = tmp[--n];
  return out;
}

int put_str(char* p, int cap, const char* s) {
  int out = 0;
  while (s != nullptr && *s != 0 && out < cap - 1) p[out++] = *s++;
  return out;
}

void write_history_raw(const char* line, int len) {
  if (len <= 0 || g_state_dir[0] == 0) {
    return;
  }
  char path[256];
  snprintf(path, sizeof(path), "%s%s", g_state_dir, kHistoryName);
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    return;
  }
  int off = 0;
  while (off < len) {
    const ssize_t w = ::write(fd, line + off, static_cast<size_t>(len - off));
    if (w > 0) {
      off += static_cast<int>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::close(fd);
}

void crash_handler(int sig) {
  // 只做异步信号安全的事：拼字符串 + open/write/close + 恢复默认动作再自杀
  char buf[256];
  int n = 0;
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, "t=");
  n += put_ulong(buf + n, static_cast<int>(sizeof(buf)) - n, uptime_s());
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, "s pid=");
  n += put_ulong(buf + n, static_cast<int>(sizeof(buf)) - n, static_cast<unsigned long>(getpid()));
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, " tag=crash sig=");
  n += put_ulong(buf + n, static_cast<int>(sizeof(buf)) - n, static_cast<unsigned long>(sig));
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, " frames=");
  n += put_ulong(buf + n, static_cast<int>(sizeof(buf)) - n, g_frames);
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, " encoded=");
  n += put_ulong(buf + n, static_cast<int>(sizeof(buf)) - n, g_encoded);
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, " temp=");
  n += put_ulong(buf + n, static_cast<int>(sizeof(buf)) - n,
                 g_temp_c < 0 ? 0UL : static_cast<unsigned long>(g_temp_c));
  n += put_str(buf + n, static_cast<int>(sizeof(buf)) - n, "C\n");
  write_history_raw(buf, n);

  // 恢复默认动作再重新触发，让进程以"被信号杀死"的状态退出（start.sh 能看出是崩溃而不是正常退出）
  signal(sig, SIG_DFL);
  raise(sig);
}

}  // namespace

unsigned diag_mask() { return g_mask; }

void diag_set_mask(unsigned mask) { g_mask = mask; }

void diag_set_state_dir(const char* dir) {
  g_state_dir[0] = 0;
  if (dir == nullptr || *dir == 0) {
    return;
  }
  snprintf(g_state_dir, sizeof(g_state_dir), "%s", dir);
  ::mkdir(g_state_dir, 0755);   // 已存在时返回 EEXIST，忽略
  // 历史文件太大就轮转一次（保留上一份，够查"上次开机"了）
  char path[300];
  snprintf(path, sizeof(path), "%s%s", g_state_dir, kHistoryName);
  struct stat st {};
  if (::stat(path, &st) == 0 && st.st_size > kRotateBytes) {
    char old[320];
    snprintf(old, sizeof(old), "%s.1", path);
    ::rename(path, old);
  }
}

const char* diag_state_dir() { return g_state_dir; }

void diag_state(const char* tag, const char* fmt, ...) {
  if (g_state_dir[0] == 0) {
    return;
  }
  char msg[512];
  msg[0] = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt == nullptr ? "" : fmt, ap);
  va_end(ap);

  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv {};
  char wall[32] = "unknown";
  if (localtime_r(&ts.tv_sec, &tmv) != nullptr) {
    strftime(wall, sizeof(wall), "%Y-%m-%dT%H:%M:%S", &tmv);
  }
  char line[768];
  const int n = snprintf(line, sizeof(line), "t=%lus wall=%s pid=%ld tag=%s %s\n",
                         uptime_s(), wall, static_cast<long>(getpid()),
                         tag == nullptr ? "?" : tag, msg);
  if (n > 0) {
    write_history_raw(line, n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1);
  }
  // 同时打一份到 stdout：手动运行/看日志时不用再去翻 state 目录
  DIAGF(kDiagBoot, "%s", line);
}

void diag_progress(unsigned long frames, unsigned long encoded, int temp_c) {
  g_frames = frames;
  g_encoded = encoded;
  g_temp_c = temp_c;
}

void diag_install_crash_handlers() {
  struct sigaction sa {};
  sa.sa_handler = crash_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;   // 只处理第一次，避免递归崩溃刷屏
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
}

int diag_cpu_temp_c() {
  const int fd = ::open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  char buf[32] = {0};
  const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) {
    return -1;
  }
  const long milli = strtol(buf, nullptr, 10);
  return static_cast<int>(milli / 1000);
}

}  // namespace gt
