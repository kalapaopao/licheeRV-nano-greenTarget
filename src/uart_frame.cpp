// 二进制帧（协议 v2）实现，见 uart_frame.h
#include "uart_frame.h"

#include <string.h>

namespace gt {
namespace {

// 显式按小端写入：不依赖编译目标自身的字节序（板端 RISC-V 与主机 x86 都是小端，
// 但协议是对外契约，写死比“碰巧一致”更安全）。
void put_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void put_f32_le(uint8_t* p, float v) {
  uint32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  p[0] = static_cast<uint8_t>(bits & 0xFFu);
  p[1] = static_cast<uint8_t>((bits >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((bits >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((bits >> 24) & 0xFFu);
}

}  // namespace

uint16_t crc16_ccitt_false(const uint8_t* p, int n) {
  uint16_t c = 0xFFFF;
  if (p == nullptr || n <= 0) {
    return c;
  }
  for (int i = 0; i < n; ++i) {
    c ^= static_cast<uint16_t>(p[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      c = (c & 0x8000u) ? static_cast<uint16_t>((c << 1) ^ 0x1021u) : static_cast<uint16_t>(c << 1);
    }
  }
  return c;
}

V2Data v2_make_data(bool detected, float yaw_deg, float pitch_deg, float v_yaw_dps,
                    float v_pitch_dps, bool dist_valid, float dist_m) {
  V2Data d;
  if (!detected) {
    // 无目标心跳帧：角度全 0、dist=-1.0，电控按「detected==0 不要使用」处理
    d.detected = 0;
    d.yaw = 0.0f;
    d.pitch = 0.0f;
    d.vyaw = 0.0f;
    d.vpitch = 0.0f;
    d.dist = -1.0f;
    return d;
  }
  d.detected = 1;
  d.yaw = yaw_deg;
  d.pitch = pitch_deg;
  d.vyaw = v_yaw_dps;
  d.vpitch = v_pitch_dps;
  d.dist = dist_valid ? dist_m : -1.0f;
  return d;
}

int build_v2_frame(const V2Data& d, uint8_t seq, uint8_t* out, int cap) {
  if (out == nullptr || cap < kV2FrameLen) {
    return 0;
  }

  out[0] = kV2Head0;
  out[1] = kV2Head1;
  out[2] = kV2Len;
  out[3] = seq;
  out[4] = kV2CmdCtrl;

  uint8_t* p = out + 5;   // 数据区起点
  p[0] = d.detected;
  put_f32_le(p + 1, d.yaw);
  put_f32_le(p + 5, d.pitch);
  put_f32_le(p + 9, d.vyaw);
  put_f32_le(p + 13, d.vpitch);
  put_f32_le(p + 17, d.dist);

  // CRC 覆盖 offset 2 起、共 3 + LEN 字节（LEN、SEQ、CMD、数据区），不含帧头
  const uint16_t crc = crc16_ccitt_false(out + 2, 3 + kV2Len);
  put_u16_le(out + 5 + kV2DataLen, crc);

  return kV2FrameLen;
}

// ---------------------------------------------------------------------------
// 反方向（电控 → 视觉，CMD=0x02）
// ---------------------------------------------------------------------------
namespace {

uint32_t get_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// IEEE754 float32 的"非有限数"就是指数域全 1（NaN / ±Inf）。看位模式，不依赖浮点比较。
bool finite_f32_bits(uint32_t u) { return ((u >> 23) & 0xFFu) != 0xFFu; }

float u32_to_f32(uint32_t u) {
  float f = 0.0f;
  memcpy(&f, &u, sizeof(f));
  return f;
}

}  // namespace

int decode_mcu_frame(const uint8_t* frame, int len, McuState* out) {
  if (frame == nullptr || out == nullptr || len != kV2McuFrameLen) {
    return -1;
  }
  if (frame[0] != kV2Head0 || frame[1] != kV2Head1 || frame[2] != kV2LenMcu ||
      frame[4] != kV2CmdMcu) {
    return -1;
  }
  const uint16_t wire = static_cast<uint16_t>(frame[kV2McuFrameLen - 2] |
                                              (frame[kV2McuFrameLen - 1] << 8));
  if (crc16_ccitt_false(frame + 2, 3 + kV2McuDataLen) != wire) {
    return -2;
  }

  const uint32_t u_roll = get_u32_le(frame + 7);
  const uint32_t u_pitch = get_u32_le(frame + 11);
  const uint32_t u_yaw = get_u32_le(frame + 15);
  const uint32_t u_gx = get_u32_le(frame + 19);
  const uint32_t u_gy = get_u32_le(frame + 23);
  const uint32_t u_gz = get_u32_le(frame + 27);
  const uint8_t state = frame[5];
  if (state > kMcuStateStop || !finite_f32_bits(u_roll) || !finite_f32_bits(u_pitch) ||
      !finite_f32_bits(u_yaw) || !finite_f32_bits(u_gx) || !finite_f32_bits(u_gy) ||
      !finite_f32_bits(u_gz)) {
    return -3;
  }

  out->seq = frame[3];
  out->state = state;
  out->flags = frame[6];
  out->roll = u32_to_f32(u_roll);
  out->pitch = u32_to_f32(u_pitch);
  out->yaw = u32_to_f32(u_yaw);
  out->gx = u32_to_f32(u_gx);
  out->gy = u32_to_f32(u_gy);
  out->gz = u32_to_f32(u_gz);
  return 0;
}

void V2StreamParser::reset() {
  have_ = 0;
  frames_ = 0;
  crc_errors_ = 0;
  len_errors_ = 0;
  other_cmd_ = 0;
  resync_bytes_ = 0;
}

void V2StreamParser::shift_one() {
  if (have_ > 0) {
    memmove(buf_, buf_ + 1, static_cast<size_t>(have_ - 1));
    --have_;
  }
}

V2StreamParser::FeedResult V2StreamParser::feed(uint8_t byte, McuState* out) {
  if (have_ >= static_cast<int>(sizeof(buf_))) {
    shift_one();
    ++resync_bytes_;
  }
  buf_[have_++] = byte;

  for (;;) {
    if (have_ == 0) {
      return kNone;
    }
    if (buf_[0] != kV2Head0) {
      shift_one();
      ++resync_bytes_;
      continue;
    }
    if (have_ >= 2 && buf_[1] != kV2Head1) {
      shift_one();
      ++resync_bytes_;
      continue;
    }
    if (have_ < 3) {
      return kNone;
    }
    const uint8_t len = buf_[2];
    if (len != kV2LenMcu && len != kV2Len) {
      shift_one();
      ++len_errors_;
      continue;
    }
    const int need = static_cast<int>(len) + 7;
    if (have_ < need) {
      return kNone;
    }
    const uint16_t wire = static_cast<uint16_t>(buf_[need - 2] | (buf_[need - 1] << 8));
    if (crc16_ccitt_false(buf_ + 2, static_cast<int>(len) + 3) != wire) {
      shift_one();
      ++crc_errors_;
      return kCrcError;
    }
    const uint8_t cmd = buf_[4];
    if (cmd == kV2CmdMcu) {
      McuState m;
      if (decode_mcu_frame(buf_, need, &m) != 0) {
        shift_one();
        return kOtherFrame;   // CRC 已通过但字段非法：丢弃，不打断后续帧
      }
      if (out != nullptr) {
        *out = m;
      }
      ++frames_;
      have_ = 0;
      return kMcuFrame;
    }
    ++other_cmd_;
    have_ = 0;
    return kOtherFrame;
  }
}

}  // namespace gt
