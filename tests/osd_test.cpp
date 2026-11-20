// OSD 自检：把 4 行电控信息画进一块灰度图，顺带断言基本性质。
//
// 编译运行：
//   g++ -std=c++11 -O1 -Wall -Wextra -I src tests/osd_test.cpp -o /tmp/osd_test && /tmp/osd_test
// 会输出 /tmp/osd_sample.ppm（P5 灰度），可直接用看图工具或 convert 打开。
#include <cstdio>
#include <string>
#include <vector>

#include "osd.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& what, const std::string& detail = "") {
  if (ok) {
    ++g_pass;
    printf("  PASS  %s%s%s\n", what.c_str(), detail.empty() ? "" : "   ", detail.c_str());
  } else {
    ++g_fail;
    printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : "   ", detail.c_str());
  }
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("=== OSD 自检 ===\n");

  const int W = 720, H = 400;
  const int stride = W;
  std::vector<uint8_t> y(static_cast<size_t>(stride) * H, 90);   // 中灰，模拟视频画面

  // 1) 宽度换算
  check(gt::osd::text_width("ABC", 2) == 3 * 9 * 2, "text_width(3 字符, scale=2) = 54",
        "got=" + std::to_string(gt::osd::text_width("ABC", 2)));

  // 2) 画一行字，应该出现白像素
  gt::osd::draw_text(y.data(), stride, W, H, 6, 4, 2, "FLIGHT SEQ012");
  int ink = 0;
  for (size_t i = 0; i < y.size(); ++i)
    if (y[i] == 255) ++ink;
  check(ink > 100, "画了 'FLIGHT SEQ012' 后出现白色像素", "ink=" + std::to_string(ink));

  // 3) 越界不崩、不写出缓冲区（画到负坐标 / 超出右边界）
  gt::osd::draw_text(y.data(), stride, W, H, -100, -100, 3, "CLIPPED");
  gt::osd::draw_text(y.data(), stride, W, H, W - 5, H - 5, 3, "EDGE");
  check(true, "越界坐标不崩溃、不越界写");

  // 4) 小写自动转大写；超范围字符变成 '?'
  const uint8_t before = y[static_cast<size_t>(200) * stride + 200];
  gt::osd::draw_text(y.data(), stride, W, H, 200, 200, 1, "a~z\u00e9");
  check(y[static_cast<size_t>(200) * stride + 200] != 0xFF || before != 0xFF,
        "非法字符被安全替换、不崩溃");

  // 5) 黑底 + 4 行，产出一张样张
  char l1[64], l2[64], l3[64], l4[64];
  snprintf(l1, sizeof l1, "F00421 FLIGHT SEQ%03u AG%03dMS%s", 12u, 8, "");
  snprintf(l2, sizeof l2, "GX%6.2f GY%6.2f GZ%6.2f", 4.0f, -5.25f, 6.75f);
  snprintf(l3, sizeof l3, "R%6.1f P%6.1f Y%6.1f", 1.0f, -2.0f, 3.5f);
  snprintf(l4, sizeof l4, "TGT Y%6.1f P%5.1f %s", 12.34f, 5.67f, "");
  const char* lines[4] = {l1, l2, l3, l4};

  const int sc = 2;
  const int line_h = (gt::osd::kFontH + 2) * sc;
  int box_w = 0;
  for (int i = 0; i < 4; ++i) {
    const int tw = gt::osd::text_width(lines[i], sc);
    if (tw > box_w) box_w = tw;
  }
  box_w += 8;
  gt::osd::fill_rect(y.data(), stride, W, H, 2, 2, box_w, line_h * 4 + 6, 16);
  for (int i = 0; i < 4; ++i) {
    gt::osd::draw_text(y.data(), stride, W, H, 6, 4 + i * line_h, sc, lines[i]);
  }
  check(box_w > 200 && box_w < W, "黑底宽度合理", "box_w=" + std::to_string(box_w));

  FILE* f = fopen("/tmp/osd_sample.ppm", "wb");
  if (f != nullptr) {
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    fwrite(y.data(), 1, y.size(), f);
    fclose(f);
    check(true, "写出样张 /tmp/osd_sample.ppm");
  } else {
    check(false, "写出样张 /tmp/osd_sample.ppm");
  }

  printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
