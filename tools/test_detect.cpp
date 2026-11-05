// 主机端自测：读 I420 原始帧 → 抽点下采样 → 跑 C++ 检测器 → 打印结果
// 用法: ./test_detect <file.i420> <w> <h> [stride]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "green_detect.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s file.i420 w h [stride]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const int W = atoi(argv[2]);
  const int H = atoi(argv[3]);
  const int S = (argc > 4) ? atoi(argv[4]) : 4;
  if (S < 1) return 2;

  FILE* fp = fopen(path, "rb");
  if (!fp) { perror("open"); return 2; }
  const size_t ysz = (size_t)W * H;
  const size_t csz = (size_t)(W / 2) * (H / 2);
  std::vector<uint8_t> i420(ysz + csz * 2);
  if (fread(i420.data(), 1, i420.size(), fp) != i420.size()) {
    fprintf(stderr, "short read: %s\n", path);
    fclose(fp);
    return 2;
  }
  fclose(fp);

  const uint8_t* Y = i420.data();
  const uint8_t* U = Y + ysz;
  const uint8_t* V = U + csz;

  // 抽点：W/H 必须能被 S 整除
  const int w = W / S;
  const int h = H / S;
  std::vector<uint8_t> sy((size_t)w * h), su((size_t)(w / 2) * (h / 2)), sv((size_t)(w / 2) * (h / 2));
  for (int yy = 0; yy < h; ++yy) {
    const int fy = yy * S;
    for (int xx = 0; xx < w; ++xx) {
      const int fx = xx * S;
      sy[(size_t)yy * w + xx] = Y[(size_t)fy * W + fx];
    }
  }
  const int cw = w / 2, chh = h / 2;
  for (int yy = 0; yy < chh; ++yy) {
    // 网格 4:2:0 色度 (cx,cy) 对应全图亮度 (2cx*S, 2cy*S)，
    // 即全图色度平面坐标 (cx*S, cy*S) —— 步长直接用 S
    const int fy = yy * S;
    for (int xx = 0; xx < cw; ++xx) {
      const int fx = xx * S;
      su[(size_t)yy * cw + xx] = U[(size_t)fy * (W / 2) + fx];
      sv[(size_t)yy * cw + xx] = V[(size_t)fy * (W / 2) + fx];
    }
  }

  greendet::Config cfg;
  greendet::Tracker tracker(cfg);
  const greendet::TrackState& st = tracker.process(sy.data(), su.data(), sv.data(), w, h);

  // 调试：打印目标位置附近的采样值（Python 版在 (621,249) → 网格 (155,62)）
  if (getenv("DUMP_SAMPLES")) {
    const int gx = 155, gy = 62;
    printf("[sample] grid(%d,%d) Y=%d U=%d V=%d | chroma_idx=%d/%zu\n", gx, gy,
           sy[(size_t)gy * w + gx], su[(size_t)(gy / 2) * (w / 2) + gx / 2],
           sv[(size_t)(gy / 2) * (w / 2) + gx / 2],
           (gy / 2) * (w / 2) + gx / 2, su.size());
  }

  const char* stat = (st.status == greendet::Status::kDetected) ? "DETECTED"
                     : (st.status == greendet::Status::kCoast) ? "COAST" : "LOST";
  printf("grid=%dx%d mask_px=%d status=%s", w, h, tracker.lastMaskPixels(), stat);
  if (st.status != greendet::Status::kLost) {
    printf("  grid_c=(%.1f,%.1f)  full_c=(%.1f,%.1f)  r_grid=%.2f r_full=%.1f sat=%.0f val=%.0f fallback=%d",
           st.cx, st.cy, st.cx * S, st.cy * S, st.blob.radius, st.blob.radius * S,
           st.blob.sat, st.blob.val, st.blob.fallback ? 1 : 0);
  }
  printf("\n");
  return st.status == greendet::Status::kLost ? 1 : 0;
}
