// 诊断层单测：打印开关、运行状态历史、崩溃归因（真跑一次 SIGSEGV，不是模拟）
//
// 编译（WSL / Linux 主机）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/diag_test.cpp src/diag.cpp
//       -o /tmp/diag_test -pthread && /tmp/diag_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "diag.h"

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

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("=== 诊断层自检 ===\n");

  // ---- 1. 打印开关 ----
  check(gt::diag_mask() == gt::kDiagAll, "默认全开（不改配置时行为与之前一致）");
  gt::diag_set_mask(0);
  check(!gt::diag(gt::kDiagBoot) && !gt::diag(gt::kDiagPerf), "mask=0 时所有分类都关");
  gt::diag_set_mask(gt::kDiagPerf | gt::kDiagMcu);
  check(gt::diag(gt::kDiagPerf) && gt::diag(gt::kDiagMcu), "打开的两个分类为真");
  check(!gt::diag(gt::kDiagBoot) && !gt::diag(gt::kDiagLog), "没打开的分类为假");
  gt::diag_set_mask(gt::kDiagAll);

  // ---- 2. 运行状态历史 ----
  char tmpl[] = "/tmp/diag_test_XXXXXX";
  char* base = mkdtemp(tmpl);
  if (base == nullptr) {
    printf("  FAIL  mkdtemp\n");
    return 1;
  }
  gt::diag_set_state_dir(base);
  check(std::string(gt::diag_state_dir()) == std::string(base), "state 目录设置成功");
  gt::diag_state("start", "print_mask=0x%X foo=%s", 0xFFu, "bar");
  gt::diag_state("exit", "rc=0 reason=normal frames=%lu", 123UL);
  std::string hist = slurp(std::string(base) + "/run_history.log");
  check(hist.find("tag=start") != std::string::npos, "写入了 tag=start 行");
  check(hist.find("print_mask=0xFF") != std::string::npos, "格式化参数正确");
  check(hist.find("tag=exit rc=0 reason=normal frames=123") != std::string::npos,
        "写入了 tag=exit 行且字段齐全");
  check(hist.find("wall=") != std::string::npos, "每行带墙钟（板子没 RTC 时是 1970）");
  check(hist.find("pid=") != std::string::npos, "每行带 pid");

  // ---- 3. 崩溃归因：子进程真的段错误一次 ----
  const std::string crash_file = std::string(base) + "/run_history.log";
  const long before = static_cast<long>(slurp(crash_file).size());
  const pid_t pid = fork();
  if (pid == 0) {
    // 子进程：装好处理器，然后触发一个真的段错误
    gt::diag_set_state_dir(base);
    gt::diag_install_crash_handlers();
    gt::diag_progress(42, 7, 55);
    raise(SIGSEGV);
    _exit(0);   // 不该走到这里
  }
  int status = 0;
  waitpid(pid, &status, 0);
  check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
        "子进程以 SIGSEGV 终止（处理器恢复默认动作后重新触发）");
  const std::string after = slurp(crash_file);
  const std::string tail = after.size() > static_cast<size_t>(before)
                               ? after.substr(static_cast<size_t>(before))
                               : std::string();
  check(tail.find("tag=crash") != std::string::npos, "崩溃前写出了 tag=crash");
  check(tail.find("sig=11") != std::string::npos, "记录了信号编号 sig=11");
  check(tail.find("frames=42") != std::string::npos, "记录了崩溃时的帧数快照");

  // ---- 4. 温度读取（不要求具体值，只要能返回 -1 或合理范围）----
  const int temp = gt::diag_cpu_temp_c();
  check(temp == -1 || (temp > -40 && temp < 150), "读温度返回 -1 或合理摄氏度",
        "temp=" + std::to_string(temp));

  printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
