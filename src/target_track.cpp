// 目标跟踪与角度换算实现，见 target_track.h
#include "target_track.h"

#include <cmath>

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;

inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// 瞄准点（检测网格坐标）：引导灯中心上方 factor × 像素直径。
// 说明：像素直径 = 2*radius*S，而 gridToAngleDeg 内部会再乘 S，
//       所以这里用网格单位 2*radius 即可，等价于像素上移 factor*2*radius*S。
inline float aim_grid_y(const CameraCalib& c, const greendet::Blob& b) {
  return b.cy - c.aim_up_offset_factor * 2.0f * b.radius;
}

}  // namespace

// ---------------------------------------------------------------------------
// 网格坐标 → 角度
//
// 检测网格是「每 S×S 个全分辨率像素取一个代表点」得到的，所以
//   全分辨率像素 = 网格坐标 × S
// 再用针孔模型反投影到角度：
//   angle_x = atan((px - cx) / fx)     正 = 目标在图像右侧
//   angle_y = atan((py - cy) / fy)     正 = 目标在图像下方（图像 y 向下）
// 最后把「图像下方」映射成「俯仰向下」，即 pitch = -angle_y。
// ---------------------------------------------------------------------------
void CameraCalib::gridToAngleDeg(float gx, float gy, int S, float* yaw, float* pitch) const {
  if (yaw == nullptr || pitch == nullptr) {
    return;
  }
  if (!has_intrinsics() || S <= 0) {
    // 未标定时退化为「像素直接当角度」，只用于链路连通性验证。
    // 此时 1 像素 = 1 度，数值不可用于闭环控制。
    *yaw = gx * static_cast<float>(S);
    *pitch = -gy * static_cast<float>(S);
    return;
  }

  const float px = gx * static_cast<float>(S);
  const float py = gy * static_cast<float>(S);
  const float angle_x_deg = std::atan((px - cx) / fx) * kRadToDeg;
  const float angle_y_deg = std::atan((py - cy) / fy) * kRadToDeg;

  float y = angle_x_deg;    // 图像右 → yaw 正
  float p = -angle_y_deg;   // 图像下 → pitch 负

  if (invert_yaw) {
    y = -y;
  }
  if (invert_pitch) {
    p = -p;
  }

  *yaw = y - optical_yaw_deg;
  *pitch = p - optical_pitch_deg;
}

// ---------------------------------------------------------------------------
// Kalman1D：匀速模型
//   x = [pos, vel]^T
//   F = [[1, dt], [0, 1]]
//   Q = q * [[dt^3/3, dt^2/2], [dt^2/2, dt]]     （连续白噪声加速度模型）
//   H = [1, 0], R = r
// ---------------------------------------------------------------------------
void Kalman1D::init(float pos, const KalmanCfg& c) {
  x_[0] = pos;
  x_[1] = 0.0f;
  P_[0][0] = c.init_pos_var;
  P_[0][1] = 0.0f;
  P_[1][0] = 0.0f;
  P_[1][1] = c.init_vel_var;
}

void Kalman1D::predict(float dt, float q) {
  if (dt <= 0.0f) {
    return;
  }

  // x = F x
  x_[0] += dt * x_[1];

  // P = F P F^T + Q
  const float p00 = P_[0][0], p01 = P_[0][1], p10 = P_[1][0], p11 = P_[1][1];
  const float dt2 = dt * dt;
  const float dt3 = dt2 * dt;

  P_[0][0] = p00 + dt * (p10 + p01) + dt2 * p11 + q * dt3 / 3.0f;
  P_[0][1] = p01 + dt * p11 + q * dt2 / 2.0f;
  P_[1][0] = p10 + dt * p11 + q * dt2 / 2.0f;
  P_[1][1] = p11 + q * dt;
}

void Kalman1D::update(float z, float r) {
  // y = z - H x
  const float innovation = z - x_[0];

  // S = H P H^T + R
  const float s = P_[0][0] + (r > 1e-9f ? r : 1e-9f);
  if (s < 1e-9f) {
    return;
  }

  // K = P H^T / S = [P00, P10]^T / S
  const float k0 = P_[0][0] / s;
  const float k1 = P_[1][0] / s;

  x_[0] += k0 * innovation;
  x_[1] += k1 * innovation;

  // P = (I - K H) P
  const float p00 = P_[0][0], p01 = P_[0][1], p10 = P_[1][0], p11 = P_[1][1];
  P_[0][0] = p00 - k0 * p00;
  P_[0][1] = p01 - k0 * p01;
  P_[1][0] = p10 - k1 * p00;
  P_[1][1] = p11 - k1 * p01;
}

// ---------------------------------------------------------------------------
// TargetTracker
// ---------------------------------------------------------------------------
void TargetTracker::configure(const CameraCalib& calib, const KalmanCfg& kf,
                              const SelectCfg& sel, float target_diameter_m) {
  calib_ = calib;
  kf_ = kf;
  sel_ = sel;
  target_diameter_m_ = target_diameter_m;
  reset();
}

void TargetTracker::reset() {
  started_ = false;
  have_last_measure_ = false;
  last_measure_ms_ = 0.0;
  out_ = TargetOut();
}

const TargetOut& TargetTracker::update(const greendet::Blob* cands, int n, int grid_S,
                                       float dt, double now_ms) {
  const float dt_use = clampf(dt, kf_.dt_min, kf_.dt_max);
  out_.candidates = (n > 0) ? n : 0;

  // ① 预测：无论本帧有没有候选，都先按上一状态推进一步
  const bool tracking = started_ && (out_.state != TargetState::kLost);
  float pred_yaw = 0.0f;
  float pred_pitch = 0.0f;
  if (tracking) {
    kf_yaw_.predict(dt_use, kf_.process_noise);
    kf_pitch_.predict(dt_use, kf_.process_noise);
    pred_yaw = kf_yaw_.pos();
    pred_pitch = kf_pitch_.pos();
  }

  // ② 候选筛选 + 加权评分，选出一个
  int best_idx = -1;
  float best_score = -1e9f;
  float best_yaw = 0.0f;
  float best_pitch = 0.0f;

  if (cands != nullptr && n > 0) {
    const float cw = clampf(sel_.conf_weight, 0.0f, 1.0f);
    const float dw = 1.0f - cw;
    const float max_dist = (sel_.max_dist_deg > 1.0f) ? sel_.max_dist_deg : 60.0f;

    for (int i = 0; i < n; ++i) {
      const greendet::Blob& b = cands[i];

      // 关卡 1：长宽比（圆形目标应接近 1）
      if (b.aspect > sel_.max_aspect) {
        continue;
      }

      // 关卡 2：质量分归一化（val 是亮度 0..255，sat 是饱和度）
      float q = clampf(b.val / 255.0f, 0.0f, 1.0f);
      // 用饱和度做一点加成：同样亮度下，越「绿」越可信
      q = clampf(0.75f * q + 0.25f * clampf(b.sat / 90.0f, 0.0f, 1.0f), 0.0f, 1.0f);

      float score = q;

      // 关卡 3：离预测位置越近，得分越高（这是「锁定」的真正机制）
      if (tracking) {
        float yaw = 0.0f, pitch = 0.0f;
        calib_.gridToAngleDeg(b.cx, aim_grid_y(calib_, b), grid_S, &yaw, &pitch);
        const float dy = yaw - pred_yaw;
        const float dp = pitch - pred_pitch;
        const float dist = std::sqrt(dy * dy + dp * dp);
        const float dist_norm = clampf(dist / max_dist, 0.0f, 1.0f);
        score = cw * q + dw * (1.0f - dist_norm);
      }

      if (score > best_score) {
        best_score = score;
        best_idx = i;
        calib_.gridToAngleDeg(b.cx, aim_grid_y(calib_, b), grid_S, &best_yaw, &best_pitch);
      }
    }
  }

  // ③ 状态机：命中 / coast / lost
  if (best_idx >= 0) {
    const greendet::Blob& b = cands[best_idx];

    if (!started_) {
      kf_yaw_.init(best_yaw, kf_);
      kf_pitch_.init(best_pitch, kf_);
      started_ = true;
    } else {
      kf_yaw_.update(best_yaw, kf_.measure_noise);
      kf_pitch_.update(best_pitch, kf_.measure_noise);
    }

    out_.misses = 0;
    ++out_.confirm_count;
    out_.state = (out_.confirm_count >= sel_.confirm_frames) ? TargetState::kConfirmed
                                                            : TargetState::kCoasting;
    out_.detected = (out_.state == TargetState::kConfirmed);

    out_.yaw_deg = kf_yaw_.pos();
    out_.pitch_deg = kf_pitch_.pos();
    out_.v_yaw_dps = kf_yaw_.vel();   // vel 已是「度/秒」（predict 里乘过 dt）
    out_.v_pitch_dps = kf_pitch_.vel();
    // 引导灯中心角度（未加瞄准偏移），只用于联调日志对照：
    // yaw/pitch 是「瞄准点」（装甲板），这两个是「灯中心」。
    calib_.gridToAngleDeg(b.cx, b.cy, grid_S, &out_.light_yaw_deg, &out_.light_pitch_deg);

    out_.has_box = true;
    out_.gx = b.cx;
    out_.gy = b.cy;
    out_.gr = b.radius;

    // 单目测距：距离 = f × 实际直径 / 像素直径
    const float diameter_px = 2.0f * b.radius * static_cast<float>(grid_S);
    const float f = 0.5f * (calib_.fx + calib_.fy);
    if (diameter_px > 0.5f && calib_.has_intrinsics() && target_diameter_m_ > 1e-6f) {
      const float z = (f * target_diameter_m_) / diameter_px;
      // 加上横向偏移后的真实斜距
      const float x = std::tan(clampf(out_.yaw_deg, -80.0f, 80.0f) / kRadToDeg) * z;
      const float y = std::tan(clampf(out_.pitch_deg, -80.0f, 80.0f) / kRadToDeg) * z;
      out_.distance_m = std::sqrt(x * x + y * y + z * z);
      out_.distance_valid = true;
    } else {
      out_.distance_m = 0.0f;
      out_.distance_valid = false;
    }

    have_last_measure_ = true;
    last_measure_ms_ = now_ms;
    ++out_.seq;
  } else if (started_) {
    ++out_.misses;
    out_.confirm_count = 0;
    // 多给 lost_frames 的余量：coast 期按预测继续输出，减少下游抖动
    const int coast_budget = sel_.lost_frames;
    if (out_.misses <= coast_budget) {
      out_.state = TargetState::kCoasting;
      out_.yaw_deg = kf_yaw_.pos();
      out_.pitch_deg = kf_pitch_.pos();
      out_.v_yaw_dps = kf_yaw_.vel();   // vel 已是「度/秒」（predict 里乘过 dt）
      out_.v_pitch_dps = kf_pitch_.vel();
      out_.detected = true;   // coast 期间仍报「有目标」，靠 age_ms 让下游判断新鲜度
      ++out_.seq;
    } else {
      out_.state = TargetState::kLost;
      out_.detected = false;
      out_.has_box = false;
      out_.v_yaw_dps = 0.0f;
      out_.v_pitch_dps = 0.0f;
      out_.distance_valid = false;
      started_ = false;
    }
  } else {
    // 从未检测到：保持 lost
    out_.state = TargetState::kLost;
    out_.detected = false;
    out_.has_box = false;
    out_.misses = 0;
    out_.confirm_count = 0;
  }

  // ④ 数据年龄：距最近一次真实测量的毫秒数
  out_.age_ms = have_last_measure_ ? static_cast<float>(now_ms - last_measure_ms_) : -1.0f;

  return out_;
}

}  // namespace gt
