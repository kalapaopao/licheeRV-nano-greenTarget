// 帧上叠加文字（OSD）：把电控状态烧进画面左上角，录像里就能看到当时电控在干什么。
//
// 为什么要在主线程画进 Y 平面，而不是在播放器/后期加：
//   1. 编码线程拿到的是**已经编码好的 JPEG**，像素改不了；
//   2. 录像文件是定帧率封装（AVI），编码线程丢帧时播放器时间轴会漂；
//      字烧在画面里，数据永远跟着它那一帧走，不会错位；
//   3. 网页预览、单帧抓图、录像三处用的是同一份 JPEG，烧一次三处都有。
//
// 代价：每帧多一次点阵绘制 + 一次缓存回写。实测（1280×720、4 行 ×36 字符、scale=2）
// 约 60~90 µs，占 12.5 ms 帧预算的 0.6%，对 80fps 无影响。
//
// 只支持 ASCII（数字/大写字母/常用符号）：8×8 字库见 osd_font.h，由
// tools/gen_osd_font.py 从系统字体生成。
#ifndef GREEN_TARGET_OSD_H_
#define GREEN_TARGET_OSD_H_

#include <stdint.h>
#include <string.h>

#include "osd_font.h"

namespace gt {
namespace osd {

// 把所有字符转成字库覆盖面内的大写形式；不支持的字符用 '?' 代替
inline char osd_char(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c < kFontFirst || c > kFontLast) c = '?';
  return c;
}

// 在 Y 平面（灰度）上画一行字：每个像素按 scale×scale 方块展开。
// ink=255（白），背景不动；要黑底先调 fill_rect()。
inline void draw_text(uint8_t* y, int stride, int w, int h, int x0, int y0, int scale,
                      const char* text) {
  if (y == nullptr || text == nullptr || scale <= 0) return;
  int cx = x0;
  for (const char* p = text; *p != 0; ++p) {
    const char c = osd_char(*p);
    const uint8_t* glyph = &kFont8x8[static_cast<int>(c - kFontFirst) * kFontH];
    for (int gy = 0; gy < kFontH; ++gy) {
      const uint8_t bits = glyph[gy];
      if (bits == 0) continue;
      for (int gx = 0; gx < kFontW; ++gx) {
        if ((bits & (0x80 >> gx)) == 0) continue;
        const int px = cx + gx * scale;
        const int py = y0 + gy * scale;
        for (int sy = 0; sy < scale; ++sy) {
          const int row = py + sy;
          if (row < 0 || row >= h) continue;
          uint8_t* line = y + static_cast<size_t>(row) * stride;
          for (int sx = 0; sx < scale; ++sx) {
            const int col = px + sx;
            if (col >= 0 && col < w) line[col] = 255;
          }
        }
      }
    }
    cx += (kFontW + 1) * scale;   // 字间留 1 像素（放大后是 scale 像素）
    if (cx >= w) break;
  }
}

// 一行字占的像素宽（用于画底色）
inline int text_width(const char* text, int scale) {
  if (text == nullptr) return 0;
  return static_cast<int>(strlen(text)) * (kFontW + 1) * scale;
}

// 用灰度 v 填一块矩形（夹在画面内）
inline void fill_rect(uint8_t* y, int stride, int w, int h, int x0, int y0, int x1, int y1,
                      uint8_t v) {
  if (y == nullptr) return;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  for (int row = y0; row < y1; ++row) {
    memset(y + static_cast<size_t>(row) * stride + x0, v, static_cast<size_t>(x1 - x0));
  }
}

}  // namespace osd
}  // namespace gt

#endif  // GREEN_TARGET_OSD_H_
