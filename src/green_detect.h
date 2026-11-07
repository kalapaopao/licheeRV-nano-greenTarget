// 绿色发光圆形目标检测 + 跟踪（板端 C++ 实现，无 OpenCV 依赖）
//
// 与 PC 版 green_target.py 的对应关系：
//   Python: HSV 阈值 → 形态学开运算 → 连通域 → 形状过滤 → 评分 → 最近邻跟踪
//   本实现: YUV 判据   → 已由抽点天然降噪 → 连通域 → 形状过滤 → 评分 → 最近邻跟踪
//
// 颜色判据为什么用 YUV 而不是 HSV：
//   1) 板端 VI 帧本来就是 YUV，转 RGB/HSV 纯属浪费 CPU；
//   2) 绿色的色度特征是"U、V 都明显低于 128"，且该判据与 NV12/NV21 的
//      U/V 字节顺序无关，不需要关心具体打包格式；
//   3) 亮度 Y 单独判"发光"，与色度解耦，逆光/过曝时仍可用。
#pragma once

#include <cstdint>
#include <vector>

namespace greendet {

// ---------------------------------------------------------------------------
// 调参（默认值按 1280x720 抽点 4 倍 → 320x180 检测网格标定）
// ---------------------------------------------------------------------------
struct Config {
  // ---- 颜色判据（YUV 域）----
  int u_max = 124;        // U < u_max 且 V < v_max ⇒ 绿色方向（128 为中性）
  int v_max = 124;
  int y_min = 100;        // 发光亮度下限（严格检测）
  int sat_min = 30;       // 饱和度下限 = sqrt((U-128)^2 + (V-128)^2)
  int y_fallback = 60;    // 回退检测（目标变暗时，仅在门控内启用）
  int sat_fallback = 25;

  // ---- 形状过滤（检测网格像素）----
  float area_min = 2.0f;
  float area_max = 400.0f;
  float fill_min = 0.50f;      // 面积 / 外接矩形
  float aspect_min = 0.45f;    // 宽 / 高
  float aspect_max = 2.20f;
  float radius_min = 1.0f;     // 等效半径
  float radius_max = 12.0f;

  // ---- 跟踪 ----
  float gate_px = 28.0f;          // 有历史时接受候选的门控半径
  float max_jump_px = 38.0f;      // 相邻帧最大跳变（超过视为不同目标）
  int coast_frames = 20;          // 丢失后允许预测保持的帧数
  float score_dist_weight = 0.02f; // 评分里距离惩罚权重（每网格像素）
  float coast_decay = 0.8f;       // coast 期间速度衰减
  float vel_smooth = 0.3f;        // 速度平滑系数（新测量占比）

  // ---- 形态学 ----
  bool open_filter = true;        // 3x3 开运算，去掉孤立噪点
};

// 单个候选
struct Blob {
  float cx = 0.0f;   // 检测网格坐标
  float cy = 0.0f;
  float area = 0.0f;
  float radius = 0.0f;
  float fill = 0.0f;
  float sat = 0.0f;
  float val = 0.0f;
  float score = 0.0f;
  float aspect = 1.0f;   // 长宽比，取长边/短边（>=1.0；圆形目标 ≈1）
  bool fallback = false;  // 是否来自回退检测
};

enum class Status { kLost, kCoast, kDetected };

struct TrackState {
  Status status = Status::kLost;
  float cx = 0.0f;    // 最近位置（网格坐标；kLost 时无意义）
  float cy = 0.0f;
  float vx = 0.0f;    // 网格像素/帧
  float vy = 0.0f;
  int misses = 0;     // 连续未命中帧数
  Blob blob{};        // 最近一次有效检测（kCoast 时为过期值）
  bool has_blob = false;
};

// ---------------------------------------------------------------------------
// 单帧检测器（无状态）
// ---------------------------------------------------------------------------
class Detector {
 public:
  explicit Detector(const Config& cfg = Config()) : cfg_(cfg) {}

  void setConfig(const Config& cfg) { cfg_ = cfg; }

  // y/u/v: 检测网格的平面数据（亮度 + 两个色度平面，尺寸 w*h / (w/2*h/2)）
  // gate: 非空时只接受中心落在该圆内的候选（跟踪门控）
  // 返回最佳候选，找不到返回 false
  bool detectFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                   int w, int h, const float* gate_cx, const float* gate_cy,
                   float gate_r, bool allow_fallback, Blob* out);

  // 收集本帧**全部**通过形状过滤的候选（不做门控），供多目标筛选使用。
  // 返回写入 out 的数量（<= max_out）。out 按 score 降序排列。
  // 说明：与 detectFrame 共用 buildMask / morphOpen，但连通域循环独立，
  //       因此不影响 detectFrame 既有行为。
  int detectMulti(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                  int w, int h, Blob* out, int max_out);

  // 统计：上一帧通过颜色阈值筛选后的像素数（调试用）
  int lastMaskPixels() const { return mask_pixels_; }

 private:
  void buildMask(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h,
                 bool relaxed, float gate_cx, float gate_cy, float gate_r, int* count);
  void morphOpen(int w, int h);

  Config cfg_;
  std::vector<uint8_t> mask_;
  std::vector<int> stack_;
  int mask_pixels_ = 0;
};

// ---------------------------------------------------------------------------
// 跟踪器（状态机 + 门控）
// ---------------------------------------------------------------------------
class Tracker {
 public:
  explicit Tracker(const Config& cfg = Config()) : cfg_(cfg), det_(cfg) {}

  void setConfig(const Config& cfg) {
    cfg_ = cfg;
    det_.setConfig(cfg);
  }

  // 处理一帧；返回跟踪状态（引用）
  const TrackState& process(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                            int w, int h);

  const Config& config() const { return cfg_; }
  int lastMaskPixels() const { return det_.lastMaskPixels(); }

 private:
  Config cfg_;
  Detector det_;
  TrackState st_{};
};

}  // namespace greendet
