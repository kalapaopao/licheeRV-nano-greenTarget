// 视觉 → 电控 UART 协议 v2（28 字节定长帧）的最小解析示例
//
// 这份代码是给电控同学直接抄的：不依赖任何库（只用 stdint/stdio/string），
// 逐字节喂数据即可，内部自动找帧头、校验 CRC、坏帧丢弃、自动重新同步。
// 协议定义见 docs/UART_PROTOCOL_V2_DESIGN.md。
//
// 用法一（当库用）：
//     V2Parser p = {0};
//     V2Frame  f;
//     for (每个收到的字节 b) {
//         if (v2_parser_feed(&p, b, &f)) {
//             // 这里拿到一整帧，f.detected == 1 时才使用 f.yaw/f.pitch
//         }
//     }
//
// 用法二（当命令行工具用，验证抓包）：
//     gcc -O2 -Wall -Wextra uart_v2_parser_example.c -o v2parse
//     v2parse < 抓包.txt
//
// 关键点（踩过坑的都在这几条）：
//   1. 帧头是 A5 5A 两个字节，数据里可能出现任意字节，所以必须"喂字节 + 自动重同步"，
//      不要用"按行读"或"等满 28 字节再解析"。
//   2. CRC 覆盖 offset 2..25（LEN/SEQ/CMD/数据），**不含**帧头 2 字节；
//      帧尾 2 字节是**小端** CRC。
//   3. detected == 0 时角度全 0、dist = -1.0f，**不要使用这些角度**。
//   4. SEQ 只用于丢包统计（每帧 +1、0..255 回绕），不能要求"必须 +1"。
//   5. 连续 300ms 收不到任何合法帧 → 视为视觉掉线，停止闭环。

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define V2_FRAME_LEN 28
#define V2_DATA_LEN 0x15

typedef struct {
  uint8_t seq;
  uint8_t cmd;
  uint8_t detected;   // 1=有目标；0=无（角度无意义）
  float yaw;          // 度，正=目标在光轴右侧
  float pitch;        // 度，正=目标在光轴上方
  float vyaw;         // 度/秒
  float vpitch;       // 度/秒
  float dist;         // 米；-1.0 = 无效
} V2Frame;

typedef struct {
  uint8_t buf[V2_FRAME_LEN];
  int n;              // 当前缓冲字节数
  uint32_t bad_crc;   // 统计：CRC 错帧数
  uint32_t resync;    // 统计：丢掉的垃圾字节数
} V2Parser;

// CRC16-CCITT-FALSE：poly 0x1021 / init 0xFFFF / 不反射（与视觉侧同一套）
static uint16_t v2_crc16(const uint8_t *p, int n) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < n; ++i) {
    c ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; ++b) c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
  }
  return c;
}

// 小端取 float（不用 memcpy 到 float 地址以外的地方，避免对齐问题）
static float v2_get_f32(const uint8_t *p) {
  uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  float f;
  memcpy(&f, &u, sizeof(f));
  return f;
}

static void v2_parser_reset(V2Parser *p) { p->n = 0; }

// 喂一个字节；凑够一整帧且 CRC 正确时填 *out 并返回 1，否则返回 0。
int v2_parser_feed(V2Parser *p, uint8_t byte, V2Frame *out) {
  if (p->n >= V2_FRAME_LEN) {          // 不该发生，兜底：丢最老的一个字节
    memmove(p->buf, p->buf + 1, V2_FRAME_LEN - 1);
    p->n = V2_FRAME_LEN - 1;
  }
  p->buf[p->n++] = byte;

  for (;;) {
    // ① 对齐帧头
    if (p->n >= 1 && p->buf[0] != 0xA5) { v2_parser_reset(p); ++p->resync; return 0; }
    if (p->n >= 2 && p->buf[1] != 0x5A) {
      memmove(p->buf, p->buf + 1, (size_t)(p->n - 1));
      --p->n;
      ++p->resync;
      continue;
    }
    if (p->n < 3) return 0;
    // ② 长度字段必须对（0x15），否则当作噪音跳过 1 字节重新找帧头
    if (p->buf[2] != V2_DATA_LEN) {
      memmove(p->buf, p->buf + 1, (size_t)(p->n - 1));
      --p->n;
      ++p->resync;
      continue;
    }
    if (p->n < V2_FRAME_LEN) return 0;
    // ③ CRC 校验（覆盖 offset 2..25，帧尾小端）
    const uint16_t crc_wire = (uint16_t)(p->buf[26] | (p->buf[27] << 8));
    if (v2_crc16(p->buf + 2, 3 + V2_DATA_LEN) != crc_wire) {
      // 坏帧：丢 1 字节重新同步（不要整段清空，后面可能紧跟一帧好的）
      memmove(p->buf, p->buf + 1, V2_FRAME_LEN - 1);
      p->n = V2_FRAME_LEN - 1;
      ++p->bad_crc;
      continue;
    }
    // ④ 解出字段
    out->seq = p->buf[3];
    out->cmd = p->buf[4];
    out->detected = p->buf[5];
    out->yaw = v2_get_f32(p->buf + 6);
    out->pitch = v2_get_f32(p->buf + 10);
    out->vyaw = v2_get_f32(p->buf + 14);
    out->vpitch = v2_get_f32(p->buf + 18);
    out->dist = v2_get_f32(p->buf + 22);
    p->n = 0;
    return 1;
  }
}

// ---------------------------------------------------------------------------
// 下面只是命令行验证工具：从标准输入读任意文本里的十六进制，解成字节喂给上面的解析器。
// 电控不需要 main，抄 #define 以上的部分即可。
// ---------------------------------------------------------------------------
static int hexval(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

int main(void) {
  V2Parser p;
  memset(&p, 0, sizeof(p));
  V2Frame f;
  int hi = -1, total = 0, frames = 0, heartbeat = 0;
  int max_zero_run = 0, run = 0;
  int ch;
  while ((ch = getchar()) != EOF) {
    const int v = hexval(ch);
    if (v < 0) { hi = -1; continue; }        // 非十六进制字符（时间戳、日志）当分隔符
    if (hi < 0) { hi = v; continue; }
    const uint8_t byte = (uint8_t)((hi << 4) | v);
    hi = -1;
    ++total;
    if (v2_parser_feed(&p, byte, &f)) {
      ++frames;
      if (f.detected) {
        run = 0;
        printf("%3d  SEQ=%-3u det=1  yaw=%7.2f pitch=%6.2f vyaw=%7.2f vpitch=%7.2f dist=%6.3f\n",
               frames, f.seq, f.yaw, f.pitch, f.vyaw, f.vpitch, f.dist);
      } else {
        ++heartbeat;
        if (++run > max_zero_run) max_zero_run = run;
        printf("%3d  SEQ=%-3u det=0  (无目标心跳帧，角度不可用)\n", frames, f.seq);
      }
    }
  }
  printf("\n字节 %d → 完整帧 %d（心跳 %d），CRC 错 %u 帧，重同步丢弃 %u 字节\n",
         total, frames, heartbeat, p.bad_crc, p.resync);
  if (frames > 0) {
    // 50Hz 下每帧 20ms，可用来判断"连续无目标"是否超过协议建议的 200ms
    printf("最长连续心跳 %d 帧 ≈ %d ms @50Hz（协议建议 >200ms 就清角度、停止闭环）\n",
           max_zero_run, max_zero_run * 20);
  }
  return frames > 0 ? 0 : 1;
}
