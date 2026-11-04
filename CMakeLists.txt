cmake_minimum_required(VERSION 3.15)

# ---------------------------------------------------------------------------
# 交叉编译设置：必须在 project() 之前指定编译器，否则 CMake 会先探测主机
# 编译器（/usr/bin/cc）并缓存，导致编不出 RISC-V 目标文件。
#
# 需要环境变量：
#   COMPILER       指向包含 riscv64-unknown-linux-musl-gcc/g++ 的 bin 目录
#   CVI_SDK_ROOT   平台 SDK 根目录（含 middleware/ osdrv/ prebuilt/ 等）
# ---------------------------------------------------------------------------
if(NOT DEFINED ENV{COMPILER} OR "$ENV{COMPILER}" STREQUAL "")
  message(FATAL_ERROR
    "环境变量 COMPILER 未设置。\n"
    "请指向包含 riscv64-unknown-linux-musl-gcc / g++ 的目录，例如：\n"
    "  export COMPILER=/path/to/host-tools/gcc/riscv64-linux-musl-x86_64/bin\n"
    "  cmake -S . -B build")
endif()
if(NOT EXISTS "$ENV{COMPILER}/riscv64-unknown-linux-musl-gcc")
  message(FATAL_ERROR
    "在 $ENV{COMPILER} 下找不到 riscv64-unknown-linux-musl-gcc。\n"
    "COMPILER 必须是工具链的 bin 目录（其中同时包含 gcc 与 g++）。")
endif()

set(CMAKE_C_COMPILER "$ENV{COMPILER}/riscv64-unknown-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "$ENV{COMPILER}/riscv64-unknown-linux-musl-g++")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(licheerv_greentarget C CXX)

# ---------------------------------------------------------------------------
# 平台 SDK：体积大、厂商版权，不随仓库分发。
# 默认在仓库上层查找，可用环境变量 CVI_SDK_ROOT 覆盖。
# ---------------------------------------------------------------------------
if(DEFINED ENV{CVI_SDK_ROOT} AND NOT "$ENV{CVI_SDK_ROOT}" STREQUAL "")
  set(CVI_SDK_ROOT "$ENV{CVI_SDK_ROOT}")
else()
  set(CVI_SDK_ROOT "${CMAKE_SOURCE_DIR}/../dartvision_build/src_work/sdk")
endif()
if(NOT EXISTS "${CVI_SDK_ROOT}/middleware/include")
  message(FATAL_ERROR
    "找不到平台 SDK：${CVI_SDK_ROOT}/middleware/include\n"
    "请设置环境变量 CVI_SDK_ROOT 指向 SDK 根目录\n"
    "（该目录下应有 middleware/ osdrv/ prebuilt/ 等子目录）。")
endif()

set(CVI_MW_DIR "${CVI_SDK_ROOT}/middleware")
set(CVI_OSDRV_DIR "${CVI_SDK_ROOT}/osdrv")

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})

# ---------------------------------------------------------------------------
# 公共编译/链接设置（派生自 CVITEK 官方 sample 的编译参数）
# ---------------------------------------------------------------------------
add_library(common_settings INTERFACE)

target_compile_options(common_settings INTERFACE
    -D__CV181X__ -O3 -march=rv64imafd -DSENSOR_GCORE_GC4653
    -D_MIDDLEWARE_V2_ -DC906 -DUSE_TPU_IVE -fsigned-char
    -Wno-format-truncation -fdiagnostics-color=always -s
)

target_include_directories(common_settings INTERFACE
    ${CMAKE_SOURCE_DIR}/src
    ${CVI_MW_DIR}/include
    ${CVI_MW_DIR}/modules/isp/include/cv181x
    ${CVI_MW_DIR}/component/isp/common
    ${CVI_MW_DIR}/sample/common
    ${CVI_MW_DIR}/v2/sample/common
    ${CVI_MW_DIR}/v2/include
    ${CVI_MW_DIR}/v2/modules/isp/include/cv181x/
    ${CVI_MW_DIR}/modules/ive/include
    ${CVI_MW_DIR}/3rdparty/
    ${CVI_OSDRV_DIR}/interdrv/v2/include/chip/cv181x/uapi
    ${CVI_OSDRV_DIR}/interdrv/include/common/uapi
)

target_link_directories(common_settings INTERFACE
    ${CVI_SDK_ROOT}/prebuilt/lib
    ${CVI_SDK_ROOT}/prebuilt/usr/lib
    ${CVI_SDK_ROOT}/prebuilt/usr/lib/3rd
)

# 只链取流/编码/ISP 需要的库：不含 TDL、RTSP、OpenCV
target_link_libraries(common_settings INTERFACE
    -lsample -lcvikernel -lcvimath -lcviruntime
    -lz -lm -lcvi_ive
    -lsys -lvpss -lcvi_bin -lvi -lvo -lgdc -lrgn -lsns_full -lisp -lvenc -lvdec
    -lawb -lae -laf -lcvi_bin_isp -lini -lmisc -lisp_algo -lpthread -latomic
)

set(APP_SRC
    src/camera.cpp
    src/green_detect.cpp
)

add_executable(camera ${APP_SRC})
target_link_libraries(camera PRIVATE common_settings)

# 板端运行库位置（与 start.sh 中设置的 LD_LIBRARY_PATH 一致）
set_target_properties(camera PROPERTIES
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd"
    BUILD_RPATH "/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd"
)
