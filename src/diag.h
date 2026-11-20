// 诊断层：分类打印开关 + 运行状态历史 + 退出/崩溃归因
//
// 解决两个现场问题：
//   1. 开机自启动后"到底跑没跑起来"——没有终端，只能事后看文件；
//   2. "为什么停了"——温度保护？VENC 报错？段错误？自启动时这些信息一刷新就没了。
//
// 三个产物（都在 $STATE_DIR 下，默认 <程序目录>/state）：
//   run_history.log   追加式时间线：supervisor/start/ready/exit/crash 一行一件事，跨重启保留
//   （start.sh 也往同一个文件写 supervisor 行，所以一份文件就能还原"这次开机发生了什么"）
//
// 打印开关（config.yaml → 环境变量）：
//   print_enable   总开关，false = 除错误/退出摘要外什么都不打
//   print_boot     启动期诊断（几何/池/传感器/VI/VENC/CAMERA_READY）
//   print_isp      曝光/黑电平/ISP 的调参与 5 秒回读
//   print_target   绿灯检测结果（GREEN_TARGET / GREEN_STAT）
//   print_perf     每 5 秒性能统计（capture_frames / DETECT / TIMING / PTS_STAT / CLOCK36_WINDOW）
//   print_uart     串口发送统计与启动行
//   print_mcu      电控下行帧（[MCU] ...）
//   print_log      记录器状态（LOG ... / LOG_SESSION_*）
//   print_avi      录像保存提示（RECORDING_SAVED）
//
// 默认全开 = 与改动前行为一致；stdout/stderr 上的错误行（Invalid ... / UART write failed 等）
// 不受开关影响，永远打印——排障时最怕的就是"把线索关掉了"。
#ifndef GREEN_TARGET_DIAG_H_
#define GREEN_TARGET_DIAG_H_

#include <stdint.h>
#include <stdio.h>

namespace gt {

// 分类位掩码（故意用 unsigned：多个分类可以同时开）
enum DiagCat : unsigned {
  kDiagBoot = 1u << 0,
  kDiagIsp = 1u << 1,
  kDiagTarget = 1u << 2,
  kDiagPerf = 1u << 3,
  kDiagUart = 1u << 4,
  kDiagMcu = 1u << 5,
  kDiagLog = 1u << 6,
  kDiagAvi = 1u << 7,
  kDiagAll = 0xFFu,
};

unsigned diag_mask();
void diag_set_mask(unsigned mask);
inline bool diag(unsigned cat) { return (diag_mask() & cat) != 0; }

// 分类打印：cat 关掉时连参数求值都省掉（printf 的开销在 80Hz 循环里不能白花）
#define DIAGF(cat, ...)          \
  do {                           \
    if (::gt::diag(cat)) {       \
      printf(__VA_ARGS__);       \
    }                            \
  } while (0)

// 运行状态目录（空 = 不写历史；测试里默认不写）
void diag_set_state_dir(const char* dir);
const char* diag_state_dir();

// 往 run_history.log 追加一行：`t=<开机秒>s wall=<墙钟> pid=<pid> tag=<tag> <msg>`
// 打开失败不报错（只读文件系统/没目录时不能因此让主程序崩）。
void diag_state(const char* tag, const char* fmt, ...);

// 崩溃处理器要用的"最后一刻"快照（异步信号安全：只读几个 int）
void diag_progress(unsigned long frames, unsigned long encoded, int temp_c);

// 安装 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT 处理器：先把 tag=crash 写进历史，再按默认动作终止
void diag_install_crash_handlers();

// 读 /sys/class/thermal/thermal_zone0/temp，返回摄氏度；失败返回 -1
int diag_cpu_temp_c();

}  // namespace gt

#endif  // GREEN_TARGET_DIAG_H_
