#!/bin/bash
# 主机端全部测试（不需要板子、不需要串口硬件）
#
#   wsl -e bash tools/run_host_tests.sh
#
# 每项打印最后一行结论；全部通过时最后输出 ALL_DONE。
cd /mnt/e/study/LicheeRV/licheeRV-nano-greenTarget || exit 1
set -e

# 带 PASS/FAIL 计数的测试：编译 + 跑，打印最后一行
run() {
  local name="$1"; shift
  local out="/tmp/${name}"
  if ! g++ -std=c++11 -O1 -Wall -Wextra -I src "$@" src/diag.cpp -o "$out" -pthread; then
    echo "BUILD FAIL: $name"; return 1
  fi
  echo "--- $name ---"
  "$out" 2>&1 | tail -1
}

# 纯模拟/需要看完整输出的测试：只要求退出码为 0
run_sim() {
  local name="$1"; shift
  local out="/tmp/${name}"
  if ! g++ -std=c++11 -O1 -Wall -Wextra -I src "$@" -o "$out" -pthread; then
    echo "BUILD FAIL: $name"; return 1
  fi
  echo "--- $name ---"
  if "$out" >/tmp/${name}.log 2>&1; then
    echo "OK（输出见 /tmp/${name}.log）"
  else
    echo "FAIL（见 /tmp/${name}.log）"; tail -5 /tmp/${name}.log
  fi
}

# ---- 算法与跟踪（不依赖任何硬件）----
run        test_phase1         tests/test_phase1.cpp src/target_track.cpp src/green_detect.cpp
run_sim    sim_kalman          tests/sim_kalman.cpp src/target_track.cpp
run_sim    sim_dart            tests/sim_dart.cpp src/target_track.cpp src/green_detect.cpp

# ---- 串口协议（pty 冒充串口，跑的是生产代码）----
run        uart_frame_test     tests/uart_frame_test.cpp src/uart_frame.cpp
run        uart_ascii          tests/uart_protocol_harness.cpp src/uart_out.cpp src/uart_frame.cpp
run        uart_bin            tests/uart_binary_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp
run        uart_rx             tests/uart_rx_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp

# ---- 飞行数据记录 ----
run        frame_log_test      tests/frame_log_test.cpp src/frame_log.cpp
run        log_trigger_pty     tests/log_trigger_pty_test.cpp src/uart_out.cpp src/uart_frame.cpp src/frame_log.cpp

# ---- 诊断层（含真实 SIGSEGV）----
run        diag_test           tests/diag_test.cpp
run        osd_test            tests/osd_test.cpp

echo ALL_DONE
