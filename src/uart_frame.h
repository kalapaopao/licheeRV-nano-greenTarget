// 二进制帧（协议 v2）的纯函数实现：CRC16 + 28 字节定长帧组装
//
// 为什么要单独一个文件：
//   1. 只依赖 <stdint.h>，不碰 termios / 板端 SDK，所以能在**主机上直接编译单测**
//      （tests/uart_frame_test.cpp；板端交叉编译不了的时候也能验证字节序与 CRC）；
//   2. 协议是「视觉 → 电控」的对外契约，把字节布局和 CRC 从线程/串口代码里隔离出来，
//      改协议时不用动 uart_out.cpp 的线程逻辑。
//
// 帧格式（与 docs/UART_PROTOCOL_V2_DESIGN.md 严格一致，小端）：
//   A5 5A | LEN(0x15) | SEQ | CMD(0x01) | 数据 21 字节 | CRC16(小端)  = 28 字节
//   CRC16 = CCITT-FALSE（poly 0x1021, init 0xFFFF, 不反射），覆盖 offset 2..(4+LEN) 共 24 字节
//
// 数据区（21 字节，偏移相对数据区起点）：
//   0  u8  detected   1=有目标 0=无
//   1  f32 yaw        度，正=右
//   5  f32 pitch      度，正=上
//   9  f32 vyaw       度/秒
//   13 f32 vpitch     度/秒
//   17 f32 dist       米，-1.0 = 无效
#ifndef GREEN_TARGET_UART_FRAME_H_
#define GREEN_TARGET_UART_FRAME_H_

#include <stdint.h>

namespace gt {

// 帧常量（电控侧解析器必须与此一致）
static const uint8_t kV2Head0 = 0xA5;
static const uint8_t kV2Head1 = 0x5A;
static const uint8_t kV2Len = 0x15;      // 数据区长度 = 21
static const uint8_t kV2CmdCtrl = 0x01;  // 控制帧
static const int kV2DataLen = 0x15;
static const int kV2FrameLen = 28;       // 2 + 1 + 1 + 1 + 21 + 2

// 数据区内容（不含帧头/长度/序号/CRC，便于单测直接填）
struct V2Data {
  uint8_t detected = 0;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float vyaw = 0.0f;
  float vpitch = 0.0f;
  float dist = -1.0f;
};

// CRC16-CCITT-FALSE：poly 0x1021, init 0xFFFF, 输入/输出均不反射
uint16_t crc16_ccitt_false(const uint8_t* p, int n);

// 按协议第 4 节「使用规则」把跟踪结果填成数据区：
//   detected==0 → 角度全 0、dist = -1.0（无论入参是什么，保证心跳帧语义固定）
//   dist_valid==false → dist = -1.0
// 把这条规则放在纯函数里（而不是散在串口线程里）是为了能在主机上单测。
V2Data v2_make_data(bool detected, float yaw_deg, float pitch_deg, float v_yaw_dps,
                    float v_pitch_dps, bool dist_valid, float dist_m);

// 组装一帧。成功返回 kV2FrameLen(28)，参数非法（out==nullptr / cap<28）返回 0。
// seq 由调用方维护（每发一帧 +1，0..255 回绕）。
int build_v2_frame(const V2Data& d, uint8_t seq, uint8_t* out, int cap);

// ---------------------------------------------------------------------------
// 反方向：电控 → 视觉（CMD=0x02 / LEN=0x1A，33 字节，100Hz）
//
// 收到的是电控的姿态与飞行状态，视觉侧**不做任何处理**，只解析 + 打到终端供联调对照。
// 字段定义见 docs/UART_PROTOCOL_V2_DESIGN.md 的「电控 → 视觉」或电控给的协议文档 §4。
// ---------------------------------------------------------------------------
static const uint8_t kV2CmdMcu = 0x02;   // 电控 → 视觉：姿态与状态
static const uint8_t kV2LenMcu = 0x1A;   // 数据区长度 = 26
static const int kV2McuDataLen = 0x1A;
static const int kV2McuFrameLen = 33;    // 2 + 1 + 1 + 1 + 26 + 2

// STATE 枚举（协议 §4.2，是枚举不是位域）
static const uint8_t kMcuStateStatic = 0;
static const uint8_t kMcuStateFlight = 1;
static const uint8_t kMcuStateStop = 2;

// FLAGS 位域（协议 §4.3）
static const uint8_t kMcuFlagImuValid = 0x01;
static const uint8_t kMcuFlagBiasReady = 0x02;
static const uint8_t kMcuFlagAccValid = 0x04;

struct McuState {
  uint8_t seq = 0;
  uint8_t state = kMcuStateStatic;   // 0 静止 / 1 飞行 / 2 停止
  uint8_t flags = 0;                 // bit0 IMU_VALID / bit1 BIAS_READY / bit2 ACC_VALID
  float roll = 0.0f;                 // 度（镖体系）
  float pitch = 0.0f;
  float yaw = 0.0f;
  float gx = 0.0f;                   // 度/秒（镖体系右手，对应 Roll）
  float gy = 0.0f;
  float gz = 0.0f;
  bool imu_valid() const { return (flags & kMcuFlagImuValid) != 0; }
};

// 解析一条完整的 33 字节 CMD=0x02 帧。
// 返回 0 = 成功；<0 = 失败原因（-1 长度/帧头/CMD 不符，-2 CRC 错，-3 字段非法）。
int decode_mcu_frame(const uint8_t* frame, int len, McuState* out);

// 字节流 → 帧 的增量解析器（供串口读取循环使用）。只认 0x02，其它 CMD 计入 other_cmd。
class V2StreamParser {
 public:
  enum FeedResult {
    kNone = 0,      // 还没凑够一帧
    kMcuFrame = 1,  // 产出电控帧（out 已填）
    kOtherFrame = 2,// 产出一帧但不是 0x02（例如回环收到自己的 0x01）
    kCrcError = 3,  // 这一帧 CRC 错，已丢弃并重新同步
  };

  FeedResult feed(uint8_t byte, McuState* out);

  unsigned long frames() const { return frames_; }
  unsigned long crc_errors() const { return crc_errors_; }
  unsigned long len_errors() const { return len_errors_; }
  unsigned long other_cmd() const { return other_cmd_; }
  unsigned long resync_bytes() const { return resync_bytes_; }
  void reset();

 private:
  void shift_one();
  uint8_t buf_[40]{};   // 最大帧 33，留余量
  int have_ = 0;
  unsigned long frames_ = 0;
  unsigned long crc_errors_ = 0;
  unsigned long len_errors_ = 0;
  unsigned long other_cmd_ = 0;
  unsigned long resync_bytes_ = 0;
};

}  // namespace gt

#endif  // GREEN_TARGET_UART_FRAME_H_
