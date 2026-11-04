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
    -D__CV181X__ -O3 -march=rv64imafdc -DSENSOR_GCORE_GC4653
    -D_MIDDLEWARE_V2_ -DC906 -DUSE_TPU_IVE -fsigned-char
    -Wno-format-truncation -fdiagnostics-color=always -s
)

target_include_directories(common_settings INTERFACE
    ${CMAKE_SOURCE_DIR}/src
    ${SDK_ROOT}/include
    ${SDK_ROOT}/include/sample
    ${SDK_ROOT}/kernel_include
    ${SDK_ROOT}/kernel_include/linux
)

target_link_directories(common_settings INTERFACE ${SDK_ROOT}/lib)

# 静态链接中间件（VI/VPSS 在 libvpu 里）
target_link_libraries(common_settings INTERFACE
    -Wl,--start-group
    -lsample -lvpu -lvenc -lisp -lisp_algo -lae -lawb -laf -lsys
    -lcvi_bin_isp -lcvi_bin -ljson-c -lini -lmisc
    -lsns_full -lsns_gc4653
    -Wl,--end-group
    -lpthread -latomic -lm
)

set(APP_SRC
    src/camera.cpp
    src/green_detect.cpp
)

add_executable(camera ${APP_SRC})
target_link_libraries(camera PRIVATE common_settings)

set_target_properties(camera PROPERTIES
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/usr/bin/lib"
    BUILD_RPATH "/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/usr/bin/lib"
)
