// 主机侧单测：v2 二进制帧的字节布局与 CRC
//
// 为什么需要它：板端交叉编译依赖 WSL 工具链，环境不可用时就无法验证；本文件只依赖
// <stdio.h>/<string.h> 和 src/uart_frame.cpp（无 POSIX 依赖），任意主机 g++ 都能跑。
// 测试向量直接取自 docs/UART_PROTOCOL_V2_DESIGN.md 第 5 节的两条示例帧——电控侧
// 也应该拿这两条做解析器自检，所以它就是双方共同的“契约测试”。
//
// 编译（Windows msys2 / Linux 主机 / 板端皆可）：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/uart_frame_test.cpp src/uart_frame.cpp -o uart_frame_test
//   然后运行 ./uart_frame_test（Windows 下为 uart_frame_test.exe）
//   Windows 实测：24 项检查全部通过（2026-11-11）。
#include <stdio.h>
#include <string.h>

#include "uart_frame.h"

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
  ++g_checks;
  if (!cond) {
    ++g_fail;
    printf("FAIL: %s\n", what);
  }
}

// 独立的逐字节 hex 比较（不调用被测代码），失败时打印两串字节便于对照
void check_bytes(const uint8_t* got, int got_len, const uint8_t* want, int want_len,
                 const char* what) {
  ++g_checks;
  if (got_len == want_len && memcmp(got, want, static_cast<size_t>(want_len)) == 0) {
    return;
  }
  ++g_fail;
  printf("FAIL: %s\n  got (%d) :", what, got_len);
  for (int i = 0; i < got_len; ++i) printf(" %02X", got[i]);
  printf("\n  want(%d) :", want_len);
  for (int i = 0; i < want_len; ++i) printf(" %02X", want[i]);
  printf("\n");
}

// 独立实现的 CCITT-FALSE（不调用被测算术），用于交叉核对协议文档里的 CRC 值
uint16_t ref_crc16(const uint8_t* p, int n) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < n; ++i) {
    c ^= static_cast<uint16_t>(p[i]) << 8;
    for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021)
                                                 : static_cast<uint16_t>(c << 1);
  }
  return c;
}

// 浮点近似比较（容差 1e-3，足够覆盖 float32 的表示误差）
bool near(float a, float b) {
  const float d = a - b;
  return (d < 1e-3f) && (d > -1e-3f);
}

void test_constants() {
  check(gt::kV2FrameLen == 28, "frame length must be 28 bytes");
  check(gt::kV2Len == 0x15, "LEN must be 0x15");
  check(gt::kV2CmdCtrl == 0x01, "CMD must be 0x01");
  check(gt::kV2Head0 == 0xA5 && gt::kV2Head1 == 0x5A, "frame head must be A5 5A");
}

void test_crc_anchor() {
  // CCITT-FALSE 的标准检查值：ASCII "123456789" → 0x29B1
  const char* s = "123456789";
  const uint16_t c = gt::crc16_ccitt_false(reinterpret_cast<const uint8_t*>(s), 9);
  ++g_checks;
  if (c != 0x29B1) {
    ++g_fail;
    printf("FAIL: crc16(\"123456789\") = 0x%04X, want 0x29B1\n", c);
  }
}

void test_doc_frame_with_target() {
  // 文档示例 1：SEQ=7 detected=1 yaw=12.34 pitch=5.67 vyaw=45.20 vpitch=-12.30 dist=3.456
  const uint8_t want[28] = {0xA5, 0x5A, 0x15, 0x07, 0x01, 0x01, 0xA4, 0x70, 0x45, 0x41,
                            0xA4, 0x70, 0xB5, 0x40, 0xCD, 0xCC, 0x34, 0x42, 0xCD, 0xCC,
                            0x44, 0xC1, 0x1B, 0x2F, 0x5D, 0x40, 0x42, 0x8E};
  const gt::V2Data d = gt::v2_make_data(true, 12.34f, 5.67f, 45.20f, -12.30f, true, 3.456f);
  uint8_t got[32];
  memset(got, 0xCC, sizeof(got));
  const int n = gt::build_v2_frame(d, 7, got, sizeof(got));
  check(n == 28, "doc frame1 length");
  check_bytes(got, n, want, 28, "doc frame1 bytes (SEQ=7, with target)");
}

void test_doc_frame_no_target() {
  // 文档示例 2：SEQ=8 detected=0 → 角度全 0、dist=-1.0
  const uint8_t want[28] = {0xA5, 0x5A, 0x15, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x99, 0x7C};
  const gt::V2Data d = gt::v2_make_data(false, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f);
  uint8_t got[32];
  memset(got, 0xCC, sizeof(got));
  const int n = gt::build_v2_frame(d, 8, got, sizeof(got));
  check(n == 28, "doc frame2 length");
  check_bytes(got, n, want, 28, "doc frame2 bytes (SEQ=8, no target)");
}

void test_no_target_forces_zero_angles() {
  // 即使调用方传了非零角度，detected==0 也必须被清零（协议第 4 节）
  const gt::V2Data d = gt::v2_make_data(false, 12.34f, 5.67f, 45.2f, -12.3f, true, 3.456f);
  check(d.detected == 0, "no-target: detected must be 0");
  check(d.yaw == 0.0f && d.pitch == 0.0f, "no-target: angles must be zeroed");
  check(d.vyaw == 0.0f && d.vpitch == 0.0f, "no-target: rates must be zeroed");
  check(d.dist == -1.0f, "no-target: dist must be -1.0");
}

void test_distance_invalid() {
  const gt::V2Data d = gt::v2_make_data(true, 1.0f, 2.0f, 3.0f, 4.0f, false, 0.0f);
  check(d.dist == -1.0f, "detected but dist invalid -> -1.0");
  const gt::V2Data e = gt::v2_make_data(true, 1.0f, 2.0f, 3.0f, 4.0f, true, 0.0f);
  check(e.dist == 0.0f, "detected and dist valid -> real value");
}

void test_crc_covers_len_to_data() {
  // CRC 必须覆盖 LEN/SEQ/CMD/数据区（offset 2..25），不含帧头
  gt::V2Data d = gt::v2_make_data(true, 12.34f, 5.67f, 45.20f, -12.30f, true, 3.456f);
  uint8_t a[32];
  const int n = gt::build_v2_frame(d, 7, a, sizeof(a));
  check(n == 28, "frame length");
  const uint16_t crc_ref = ref_crc16(a + 2, 3 + a[2]);
  const uint16_t crc_wire = static_cast<uint16_t>(a[26] | (a[27] << 8));
  ++g_checks;
  if (crc_ref != crc_wire) {
    ++g_fail;
    printf("FAIL: CRC mismatch ref=0x%04X wire=0x%04X\n", crc_ref, crc_wire);
  }
  // 不含帧头的证明：帧头字节不参与 CRC，篡改帧头不应改变 CRC 位置的值
  uint8_t b[32];
  memcpy(b, a, sizeof(a));
  b[0] = 0x00;
  check(ref_crc16(b + 2, 3 + b[2]) == crc_wire, "CRC must not cover the 2 header bytes");
}

void test_seq_changes_crc() {
  gt::V2Data d = gt::v2_make_data(true, 1.0f, 2.0f, 3.0f, 4.0f, true, 5.0f);
  uint8_t a[32], b[32];
  gt::build_v2_frame(d, 1, a, sizeof(a));
  gt::build_v2_frame(d, 2, b, sizeof(b));
  check(a[3] == 1 && b[3] == 2, "SEQ field placed at offset 3");
  check((a[26] != b[26]) || (a[27] != b[27]), "SEQ must be covered by CRC");
}

void test_buffer_guards() {
  gt::V2Data d = gt::v2_make_data(false, 0, 0, 0, 0, false, 0);
  uint8_t small[27];
  check(gt::build_v2_frame(d, 0, small, sizeof(small)) == 0, "cap<28 must return 0");
  check(gt::build_v2_frame(d, 0, nullptr, 28) == 0, "nullptr out must return 0");
}

void test_seq_wraps() {
  // seq 是 u8：255 之后回到 0（由调用方维护，这里确认函数只做透传）
  gt::V2Data d = gt::v2_make_data(false, 0, 0, 0, 0, false, 0);
  uint8_t a[32];
  gt::build_v2_frame(d, 255, a, sizeof(a));
  check(a[3] == 255, "seq=255 passes through");
  gt::build_v2_frame(d, 0, a, sizeof(a));
  check(a[3] == 0, "seq=0 passes through");
}

// ---------------------------------------------------------------------------
// 反方向（电控 → 视觉，CMD=0x02 / 33 字节）：解码 + 流式解析
// ---------------------------------------------------------------------------
// 电控文档 §6.3：SEQ=7 STATE=1 FLAGS=0x07 roll=1.0 pitch=-2.0 yaw=3.5 gx=4.0 gy=-5.25 gz=6.75
static const uint8_t kMcuFrame[] = {0xA5, 0x5A, 0x1A, 0x07, 0x02, 0x01, 0x07, 0x00, 0x00, 0x80,
                                    0x3F, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x60, 0x40, 0x00,
                                    0x00, 0x80, 0x40, 0x00, 0x00, 0xA8, 0xC0, 0x00, 0x00, 0xD8,
                                    0x40, 0x73, 0x88};
// 电控文档 §6.4：SEQ=0 STATE=0 FLAGS=0 全零心跳
static const uint8_t kMcuIdle[] = {0xA5, 0x5A, 0x1A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x1F, 0x38};

void test_decode_mcu_frame() {
  check(gt::kV2McuFrameLen == 33, "mcu frame length must be 33");

  gt::McuState m;
  check(gt::decode_mcu_frame(kMcuFrame, 33, &m) == 0, "decode doc mcu frame ok");
  check(m.seq == 7 && m.state == gt::kMcuStateFlight, "mcu seq/state");
  check(m.flags == 0x07 && m.imu_valid(), "mcu flags/imu_valid");
  check(near(m.roll, 1.0f) && near(m.pitch, -2.0f) && near(m.yaw, 3.5f), "mcu angles");
  check(near(m.gx, 4.0f) && near(m.gy, -5.25f) && near(m.gz, 6.75f), "mcu gyro");

  check(gt::decode_mcu_frame(kMcuIdle, 33, &m) == 0, "decode idle frame ok");
  check(m.state == 0 && m.flags == 0 && near(m.yaw, 0.0f), "idle frame all zero");

  // 失败路径
  check(gt::decode_mcu_frame(kMcuFrame, 32, &m) == -1, "wrong length rejected");
  uint8_t bad[33];
  memcpy(bad, kMcuFrame, 33);
  bad[10] ^= 0x01;
  check(gt::decode_mcu_frame(bad, 33, &m) == -2, "bad crc rejected");
  memcpy(bad, kMcuFrame, 33);
  bad[5] = 3; /* STATE 只允许 0/1/2 */
  bad[31] = 0;
  bad[32] = 0;
  const uint16_t c = gt::crc16_ccitt_false(bad + 2, 29);
  bad[31] = static_cast<uint8_t>(c & 0xFF);
  bad[32] = static_cast<uint8_t>(c >> 8);
  check(gt::decode_mcu_frame(bad, 33, &m) == -3, "invalid state rejected");
}

void test_stream_parser() {
  gt::V2StreamParser p;
  gt::McuState m;

  // 逐字节喂一整帧
  int got = 0;
  for (int i = 0; i < 33; ++i) {
    if (p.feed(kMcuFrame[i], &m) == gt::V2StreamParser::kMcuFrame) {
      ++got;
    }
  }
  check(got == 1 && p.frames() == 1, "stream parser: byte-by-byte yields 1 frame");
  check(near(m.yaw, 3.5f), "stream parser decoded yaw");

  // 两帧连收（先喂满第一帧，再喂第二帧）
  gt::V2StreamParser p2;
  got = 0;
  for (int i = 0; i < 33; ++i) {
    if (p2.feed(kMcuFrame[i], &m) == gt::V2StreamParser::kMcuFrame) ++got;
  }
  for (int i = 0; i < 33; ++i) {
    if (p2.feed(kMcuIdle[i], &m) == gt::V2StreamParser::kMcuFrame) ++got;
  }
  check(got == 2 && p2.frames() == 2, "stream parser: 2 frames back-to-back");

  // 垃圾前缀 + 好帧
  gt::V2StreamParser p3;
  const uint8_t junk[5] = {0x00, 0xA5, 0x11, 0x5A, 0x33};
  for (int i = 0; i < 5; ++i) p3.feed(junk[i], &m);
  got = 0;
  for (int i = 0; i < 33; ++i) {
    if (p3.feed(kMcuFrame[i], &m) == gt::V2StreamParser::kMcuFrame) ++got;
  }
  check(got == 1 && p3.resync_bytes() > 0, "stream parser: resync after junk");

  // 坏 CRC：报 kCrcError，随后好帧仍能解出
  gt::V2StreamParser p4;
  uint8_t broken[33];
  memcpy(broken, kMcuFrame, 33);
  broken[20] ^= 0xFF;
  bool saw_crc = false;
  for (int i = 0; i < 33; ++i) {
    if (p4.feed(broken[i], &m) == gt::V2StreamParser::kCrcError) saw_crc = true;
  }
  got = 0;
  for (int i = 0; i < 33; ++i) {
    if (p4.feed(kMcuFrame[i], &m) == gt::V2StreamParser::kMcuFrame) ++got;
  }
  check(saw_crc && p4.crc_errors() > 0, "stream parser: crc error detected");
  check(got == 1, "stream parser: recovers after crc error");

  // 回环收到自己的 0x01 帧：算 other，不产生电控数据
  gt::V2StreamParser p5;
  const gt::V2Data d = gt::v2_make_data(true, 1.0f, 2.0f, 3.0f, 4.0f, true, 5.0f);
  uint8_t own[28];
  gt::build_v2_frame(d, 9, own, sizeof(own));
  bool other = false;
  for (int i = 0; i < 28; ++i) {
    if (p5.feed(own[i], &m) == gt::V2StreamParser::kOtherFrame) other = true;
  }
  check(other && p5.other_cmd() == 1 && p5.frames() == 0, "stream parser: 0x01 echo counted as other");
}

}  // namespace

int main() {
  test_constants();
  test_crc_anchor();
  test_doc_frame_with_target();
  test_doc_frame_no_target();
  test_no_target_forces_zero_angles();
  test_distance_invalid();
  test_crc_covers_len_to_data();
  test_seq_changes_crc();
  test_buffer_guards();
  test_seq_wraps();
  test_decode_mcu_frame();
  test_stream_parser();
  printf("uart_frame_test: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
