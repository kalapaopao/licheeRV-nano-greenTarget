// detect_cli — 绿色圆灯检测的命令行工具（用于板端验证，零第三方依赖）
//
// 输入：I420 原始帧文件（yyyy...uu...vv...），参数给出宽高与抽点步长
// 输出：检测状态、网格坐标、换算回原图的坐标与半径、耗时
//
// 交叉编译（静态链接，板端零依赖）：
//   riscv64-unknown-linux-musl-g++ -O2 -std=c++11 -static \
//       -o detect_cli tools/detect_cli.cpp src/green_detect.cpp -lm
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "../src/green_detect.h"

static double now_ms() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s frame.i420 <w> <h> [stride=4] [repeat=1]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const int W = atoi(argv[2]);
  const int H = atoi(argv[3]);
  const int S = (argc > 4) ? atoi(argv[4]) : 4;
  const int repeat = (argc > 5) ? atoi(argv[5]) : 1;
  if (W <= 0 || H <= 0 || S <= 0 || W % S || H % S) {
    fprintf(stderr, "bad args\n");
    return 2;
  }

  FILE* fp = fopen(path, "rb");
  if (!fp) { perror("open"); return 2; }
  const size_t ysz = (size_t)W * H;
  const size_t csz = (size_t)(W / 2) * (H / 2);
  std::vector<unsigned char> i420(ysz + csz * 2);
  if (fread(i420.data(), 1, i420.size(), fp) != i420.size()) {
    fprintf(stderr, "short read\n");
    fclose(fp);
    return 2;
  }
  fclose(fp);

  const unsigned char* Y = i420.data();
  const unsigned char* U = Y + ysz;
  const unsigned char* V = U + csz;

  // 抽点：亮度每 S 取 1；色度平面也每 S 取 1（保证与网格 4:2:0 对齐）
  const int w = W / S, h = H / S;
  const int cw = w / 2, ch = h / 2;
  std::vector<unsigned char> sy((size_t)w * h), su((size_t)cw * ch), sv((size_t)cw * ch);
  for (int yy = 0; yy < h; ++yy) {
    const int fy = yy * S;
    for (int xx = 0; xx < w; ++xx) sy[(size_t)yy * w + xx] = Y[(size_t)fy * W + xx * S];
  }
  for (int yy = 0; yy < ch; ++yy) {
    const int fy = yy * S;
    for (int xx = 0; xx < cw; ++xx) {
      const int fx = xx * S;
      su[(size_t)yy * cw + xx] = U[(size_t)fy * (W / 2) + fx];
      sv[(size_t)yy * cw + xx] = V[(size_t)fy * (W / 2) + fx];
    }
  }

  greendet::Tracker tracker;
  const greendet::TrackState* st = nullptr;
  double t0 = now_ms();
  for (int i = 0; i < repeat; ++i) st = &tracker.process(sy.data(), su.data(), sv.data(), w, h);
  double elapsed = (now_ms() - t0) / (repeat > 0 ? repeat : 1);

  const char* name = (st->status == greendet::Status::kDetected) ? "DETECTED"
                     : (st->status == greendet::Status::kCoast) ? "COAST" : "LOST";
  printf("grid=%dx%d mask_px=%d status=%s", w, h, tracker.lastMaskPixels(), name);
  if (st->status != greendet::Status::kLost) {
    printf(" grid_c=(%.1f,%.1f) full_c=(%.1f,%.1f) r_full=%.1f sat=%.0f val=%.0f fb=%d",
           st->cx, st->cy, st->cx * S, st->cy * S, st->blob.radius * S, st->blob.sat,
           st->blob.val, st->blob.fallback ? 1 : 0);
  }
  printf("  detect_ms=%.3f\n", elapsed);
  return st->status == greendet::Status::kLost ? 1 : 0;
}
