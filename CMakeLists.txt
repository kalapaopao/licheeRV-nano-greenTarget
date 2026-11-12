cmake_minimum_required(VERSION 3.15)

# ---------------------------------------------------------------------------
# fly_LicheeRV：GC4653 80FPS 取流 + 绿色圆灯检测
#
# 交叉编译（必须在 project() 之前指定编译器）：
#   COMPILER        交叉工具链 bin 目录
#   CVI_SDK_ROOT    Sipeed 官方 SDK 产物目录（含 include/ lib/ kernel_include/）
# ---------------------------------------------------------------------------
if(NOT DEFINED ENV{COMPILER} OR "$ENV{COMPILER}" STREQUAL "")
  message(FATAL_ERROR "环境变量 COMPILER 未设置（指向 riscv64-unknown-linux-musl-gcc 所在 bin 目录）")
endif()
if(NOT EXISTS "$ENV{COMPILER}/riscv64-unknown-linux-musl-gcc")
  message(FATAL_ERROR "在 $ENV{COMPILER} 下找不到 riscv64-unknown-linux-musl-gcc")
endif()

set(CMAKE_C_COMPILER "$ENV{COMPILER}/riscv64-unknown-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "$ENV{COMPILER}/riscv64-unknown-linux-musl-g++")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

project(licheerv_greentarget C CXX)

# SDK 产物目录：默认用同工作区下的归档
if(DEFINED ENV{CVI_SDK_ROOT} AND NOT "$ENV{CVI_SDK_ROOT}" STREQUAL "")
  set(SDK_ROOT "$ENV{CVI_SDK_ROOT}")
else()
  set(SDK_ROOT "${CMAKE_SOURCE_DIR}/../env/sipeed-sdk-build")
endif()
if(NOT EXISTS "${SDK_ROOT}/include")
  message(FATAL_ERROR "找不到 SDK 产物：${SDK_ROOT}/include")
endif()

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})

add_library(common_settings INTERFACE)

# 与原版一致的编译参数（-march 用 cv181x 的 xthead 变体以匹配 SDK）
target_compile_options(common_settings INTERFACE
    -D__CV181X__ -DARCH_CV181X -O3 -ffunction-sections -fdata-sections -march=rv64imafdcv0p7xthead -DSENSOR_GCORE_GC4653
    -D_MIDDLEWARE_V2_ -DC906 -DUSE_TPU_IVE -fsigned-char
    -Wno-format-truncation -fdiagnostics-color=always -s
)

target_include_directories(common_settings INTERFACE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${SDK_ROOT}/include
    ${SDK_ROOT}/include/sample
    ${SDK_ROOT}/kernel_include
    ${SDK_ROOT}/kernel_include/linux
)

target_link_directories(common_settings INTERFACE ${SDK_ROOT}/lib_dyn ${SDK_ROOT}/lib ${SDK_ROOT}/lib/3rd)

# 动态链接板端自带中间件（与原版 80fps 工程的 sdk/board-libs 方式一致）。
# 板端 /mnt/system/usr/lib 里就有这些 .so，运行时由 start.sh 的 LD_LIBRARY_PATH 提供。
target_link_libraries(common_settings INTERFACE
    -Wl,--gc-sections
    -lsys -lvpu -lvenc -lisp -lisp_algo -lae -lawb -laf
    -lcvi_bin -lcvi_bin_isp
    -lini -lpthread -latomic -lm
)

set(APP_SRC
    src/camera.cpp
    src/green_detect.cpp
    # 阶段1：多目标筛选 + 卡尔曼（角度域）+ 串口输出
    src/target_track.cpp
    src/uart_out.cpp
    # 串口协议 v2 的定长二进制帧（纯函数，无 POSIX/SDK 依赖，可主机单测）
    src/uart_frame.cpp
    # 用户态 GC4653 驱动（含 720P_60FPS 模式表 + 1080P timing），替代 libsns_gc4653.a
    src/sensor/gc4653_cmos.c
    src/sensor/gc4653_sensor_ctl.c
    # 原版 80fps 工程验证过的 sample 层（720P 模式按 1920x1080 配置）
    src/common/sample_common_bin.c
    src/common/sample_common_isp.c
    src/common/sample_common_platform.c
    src/common/sample_common_sensor.c
    src/common/sample_common_sys.c
    src/common/sample_common_vi.c
    src/common/sample_common_vpss.c
    src/common/atomic_compat.c
)

add_executable(camera ${APP_SRC})
target_link_libraries(camera PRIVATE common_settings)

set_target_properties(camera PROPERTIES
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/usr/bin/lib"
    BUILD_RPATH "/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/usr/bin/lib"
)
