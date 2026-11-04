// 绿色发光圆形目标检测 + 跟踪实现，见 green_detect.h
#include "green_detect.h"

#include <cmath>
#include <cstring>

namespace greendet {
namespace {

inline int uv_dist2(int u, int v) {
  const int du = u - 128;
  const int dv = v - 128;
  return du * du + dv * dv;
}

}  // namespace

// ---------------------------------------------------------------------------
// 掩膜生成：颜色判据（严格 / 回退）
// ---------------------------------------------------------------------------
void Detector::buildMask(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h,
                         bool relaxed, float gate_cx, float gate_cy, float gate_r,
                         int* count) {
  const int y_min = relaxed ? cfg_.y_fallback : cfg_.y_min;
  const int sat_min = relaxed ? cfg_.sat_fallback : cfg_.sat_min;
  const int sat_min2 = sat_min * sat_min;
  const bool use_gate = !relaxed && gate_r > 0.0f;
  const float gate_r2 = gate_r * gate_r;

  mask_.assign(static_cast<size_t>(w) * h, 0);
  int n = 0;
  const int cw = w >> 1;

  for (int yy = 0; yy < h; ++yy) {
    const uint8_t* yrow = y + static_cast<size_t>(yy) * w;
    // 4:2:0 每两行共享一行色度
    const uint8_t* urow = u + static_cast<size_t>(yy >> 1) * cw;
    const uint8_t* vrow = v + static_cast<size_t>(yy >> 1) * cw;
    uint8_t* mrow = mask_.data() + static_cast<size_t>(yy) * w;
    for (int xx = 0; xx < w; ++xx) {
      if (yrow[xx] < y_min) {
        continue;
      }
      const int ci = xx >> 1;
      const int uu = urow[ci];
      const int vv = vrow[ci];
      // 绿色方向：U、V 都低于中性值 128
      if (uu > cfg_.u_max || vv > cfg_.v_max) {
        continue;
      }
      if (uv_dist2(uu, vv) < sat_min2) {
        continue;
      }
      if (use_gate) {
        const float dx = static_cast<float>(xx) - gate_cx;
        const float dy = static_cast<float>(yy) - gate_cy;
        if (dx * dx + dy * dy > gate_r2) {
          continue;
        }
      }
      mrow[xx] = 1;
      ++n;
    }
  }
  *count = n;
}

// ---------------------------------------------------------------------------
// 3x3 开运算：先腐蚀后膨胀，去掉孤立单点（抽点后噪点多为 1~2 像素）
// ---------------------------------------------------------------------------
void Detector::morphOpen(int w, int h) {
  if (!cfg_.open_filter) {
    return;
  }
  static thread_local std::vector<uint8_t> tmp;
  tmp.assign(mask_.size(), 0);

  // 腐蚀
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      const size_t idx = static_cast<size_t>(yy) * w + xx;
      if (!mask_[idx]) {
        continue;
      }
      bool keep = true;
      for (int dy = -1; dy <= 1 && keep; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int ny = yy + dy;
          const int nx = xx + dx;
          if (ny < 0 || ny >= h || nx < 0 || nx >= w || !mask_[static_cast<size_t>(ny) * w + nx]) {
            keep = false;
            break;
          }
        }
      }
      tmp[idx] = keep ? 1 : 0;
    }
  }
  // 膨胀
  mask_.assign(mask_.size(), 0);
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      if (!tmp[static_cast<size_t>(yy) * w + xx]) {
        continue;
      }
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int ny = yy + dy;
          const int nx = xx + dx;
          if (ny >= 0 && ny < h && nx >= 0 && nx < w) {
            mask_[static_cast<size_t>(ny) * w + nx] = 1;
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 单帧检测：连通域 + 形状过滤 + 评分选优
// ---------------------------------------------------------------------------
bool Detector::detectFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h,
                           const float* gate_cx, const float* gate_cy, float gate_r,
                           bool allow_fallback, Blob* out) {
  if (out == nullptr || y == nullptr || u == nullptr || v == nullptr || w <= 2 || h <= 2) {
    return false;
  }

  int n = 0;
  buildMask(y, u, v, w, h, false, gate_cx ? *gate_cx : 0.0f, gate_cy ? *gate_cy : 0.0f,
            gate_cx ? gate_r : 0.0f, &n);
  mask_pixels_ = n;

  bool fallback_used = false;
  if (n == 0 && allow_fallback && gate_cx != nullptr) {
    // 目标变暗：仅在门控邻域内放宽阈值重试（对应 Python 版的回退检测）
    buildMask(y, u, v, w, h, true, *gate_cx, *gate_cy, gate_r, &n);
    mask_pixels_ = n;
    fallback_used = true;
  }
  if (n == 0) {
    return false;
  }

  morphOpen(w, h);

  // ---- 连通域（8 邻域，迭代栈，避免递归爆栈）----
  static thread_local std::vector<int> visited;
  visited.assign(mask_.size(), 0);
  stack_.clear();

  bool found = false;
  Blob best;
  best.score = -1e9f;
  const float cw = static_cast<float>(w);
  const float ch = static_cast<float>(h);
  const float diag = std::sqrt(cw * cw + ch * ch);

  for (int sy = 0; sy < h; ++sy) {
    for (int sx = 0; sx < w; ++sx) {
      const size_t sidx = static_cast<size_t>(sy) * w + sx;
      if (!mask_[sidx] || visited[sidx]) {
        continue;
      }
      // 泛洪
      stack_.push_back(static_cast<int>(sidx));
      visited[sidx] = 1;
      int area = 0;
      int minx = sx, maxx = sx, miny = sy, maxy = sy;
      double sumx = 0.0, sumy = 0.0;
      while (!stack_.empty()) {
        const int cur = stack_.back();
        stack_.pop_back();
        const int cy_ = cur / w;
        const int cx_ = cur % w;
        ++area;
        sumx += cx_;
        sumy += cy_;
        if (cx_ < minx) minx = cx_;
        if (cx_ > maxx) maxx = cx_;
        if (cy_ < miny) miny = cy_;
        if (cy_ > maxy) maxy = cy_;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int ny = cy_ + dy;
            const int nx = cx_ + dx;
            if (ny < 0 || ny >= h || nx < 0 || nx >= w) {
              continue;
            }
            const size_t nidx = static_cast<size_t>(ny) * w + nx;
            if (mask_[nidx] && !visited[nidx]) {
              visited[nidx] = 1;
              stack_.push_back(static_cast<int>(nidx));
            }
          }
        }
      }

      // ---- 形状过滤 ----
      const float farea = static_cast<float>(area);
      if (farea < cfg_.area_min || farea > cfg_.area_max) {
        continue;
      }
      const int bw = maxx - minx + 1;
      const int bh = maxy - miny + 1;
      const float bbox = static_cast<float>(bw) * static_cast<float>(bh);
      if (bbox <= 0.0f) {
        continue;
      }
      const float fill = farea / bbox;
      if (fill < cfg_.fill_min) {
        continue;
      }
      const float aspect = static_cast<float>(bw) / static_cast<float>(bh);
      if (aspect < cfg_.aspect_min || aspect > cfg_.aspect_max) {
        continue;
      }
      const float radius = std::sqrt(farea / 3.14159265f);
      if (radius < cfg_.radius_min || radius > cfg_.radius_max) {
        continue;
      }

      const float ccx = static_cast<float>(sumx / area);
      const float ccy = static_cast<float>(sumy / area);

      // 门控（严格检测时已在掩膜里限制，这里再兜一次；回退检测必须限制）
      float dist = 0.0f;
      if (gate_cx != nullptr) {
        const float dx = ccx - *gate_cx;
        const float dy = ccy - *gate_cy;
        dist = std::sqrt(dx * dx + dy * dy);
        if (dist > gate_r) {
          continue;
        }
      }

      // ---- 评分：亮度 + 饱和度 − 距离惩罚 ----
      const int ci = (static_cast<int>(ccy) >> 1) * (w >> 1) + (static_cast<int>(ccx) >> 1);
      const int uu = u[ci];
      const int vv = v[ci];
      const int yy = y[static_cast<int>(ccy) * w + static_cast<int>(ccx)];
      const float sat = std::sqrt(static_cast<float>(uv_dist2(uu, vv)));
      float score = static_cast<float>(yy) + 0.5f * sat;
      if (gate_cx != nullptr && diag > 0.0f) {
        score -= cfg_.score_dist_weight * dist * 10.0f;
      }

      if (score > best.score) {
        best.score = score;
        best.cx = ccx;
        best.cy = ccy;
        best.area = farea;
        best.radius = radius;
        best.fill = fill;
        best.sat = sat;
        best.val = static_cast<float>(yy);
        best.fallback = fallback_used;
        found = true;
      }
    }
  }

  if (found) {
    *out = best;
  }
  return found;
}

// ---------------------------------------------------------------------------
// 跟踪：预测 → 门控检测 → 更新/coast → 超时丢失
// ---------------------------------------------------------------------------
const TrackState& Tracker::process(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w,
                                   int h) {
  const bool tracking = (st_.status == Status::kDetected || st_.status == Status::kCoast);

  float gate_cx = 0.0f;
  float gate_cy = 0.0f;
  float gate_r = 0.0f;
  const float* pgate_cx = nullptr;
  const float* pgate_cy = nullptr;
  if (tracking) {
    gate_cx = st_.cx + st_.vx;
    gate_cy = st_.cy + st_.vy;
    gate_r = cfg_.gate_px * (1.0f + 0.3f * static_cast<float>(st_.misses));
    pgate_cx = &gate_cx;
    pgate_cy = &gate_cy;
  }

  Blob blob;
  bool hit = det_.detectFrame(y, u, v, w, h, pgate_cx, pgate_cy, gate_r, tracking, &blob);

  // 跳变保护：超过 max_jump 视为不同目标，拒绝该候选
  if (hit && tracking) {
    const float dx = blob.cx - st_.cx;
    const float dy = blob.cy - st_.cy;
    if (std::sqrt(dx * dx + dy * dy) > cfg_.max_jump_px) {
      hit = false;
    }
  }

  if (hit) {
    if (tracking) {
      const float nvx = blob.cx - st_.cx;
      const float nvy = blob.cy - st_.cy;
      st_.vx = (1.0f - cfg_.vel_smooth) * st_.vx + cfg_.vel_smooth * nvx;
      st_.vy = (1.0f - cfg_.vel_smooth) * st_.vy + cfg_.vel_smooth * nvy;
    } else {
      st_.vx = 0.0f;
      st_.vy = 0.0f;
    }
    st_.cx = blob.cx;
    st_.cy = blob.cy;
    st_.misses = 0;
    st_.status = Status::kDetected;
    st_.blob = blob;
    st_.has_blob = true;
  } else if (tracking) {
    ++st_.misses;
    if (st_.misses <= cfg_.coast_frames) {
      // 只预测不更新：短时遮挡/变暗时保持输出连续
      st_.status = Status::kCoast;
      st_.cx += st_.vx;
      st_.cy += st_.vy;
      st_.vx *= cfg_.coast_decay;
      st_.vy *= cfg_.coast_decay;
    } else {
      st_.status = Status::kLost;
      st_.has_blob = false;
      st_.vx = 0.0f;
      st_.vy = 0.0f;
    }
  } else {
    st_.status = Status::kLost;
    st_.has_blob = false;
  }

  return st_;
}

}  // namespace greendet
