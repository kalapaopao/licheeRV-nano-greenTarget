// 针对真实飞镖场景的评估仿真
//   参数来自实际：目标横向往返 0.5~0.6m、匀速慢速；飞镖会滚转
//   对比：滚转补偿前后 / 卡尔曼 vs OneEuro vs 直通
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <algorithm>

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.0f / kPi;
constexpr float kRad = kPi / 180.0f;
const float FPS = 80.0f;
const float DT = 1.0f / FPS;

// ---- One Euro Filter（1 维），照深大/论文的实现 ----
class OneEuro {
 public:
  OneEuro(float min_cutoff, float beta, float d_cutoff)
      : min_cutoff_(min_cutoff), beta_(beta), d_cutoff_(d_cutoff) {}
  float filter(float x, float dt) {
    const float a_d = alpha(dt, d_cutoff_);
    const float dx = (x - x_pre_) / dt;
    dx_hat_ = a_d * dx + (1.0f - a_d) * dx_hat_;
    const float cut = min_cutoff_ + beta_ * std::fabs(dx_hat_);
    const float a = alpha(dt, cut);
    x_hat_ = a * x + (1.0f - a) * x_pre_;
    x_pre_ = x_hat_;
    return x_hat_;
  }
  float vel() const { return dx_hat_; }
  void reset(float x) { x_pre_ = x; x_hat_ = x; dx_hat_ = 0.0f; }

 private:
  static float alpha(float dt, float cutoff) {
    const float r = 2.0f * kPi * cutoff * dt;
    return r / (r + 1.0f);
  }
  float min_cutoff_, beta_, d_cutoff_;
  float x_pre_ = 0.0f, x_hat_ = 0.0f, dx_hat_ = 0.0f;
};

struct Rng {
  unsigned s;
  explicit Rng(unsigned seed) : s(seed) {}
  float uni() {   // [-1, 1]
    s = s * 1103515245u + 12345u;
    return ((s >> 16) & 0x7FFF) / 16384.0f - 1.0f;
  }
};
}  // namespace

int main() {
  // =====================================================================
  printf("=========== 场景1：滚转导致的角度污染（最关键）===========\n");
  printf("目标真实偏角固定 5°；飞镖以不同角速度滚转；观察测到的 yaw/pitch\n\n");
  printf("%-12s %22s %22s\n", "滚转角速度", "不补偿：yaw/pitch 抖动", "补偿后：yaw/pitch 抖动");
  printf("------------------------------------------------------------------------\n");

  for (float roll_rate : {0.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f}) {
    const float true_yaw = 5.0f, true_pitch = 2.0f;
    float roll = 0.0f;
    float yaw_min = 1e9f, yaw_max = -1e9f, p_min = 1e9f, p_max = -1e9f;
    float cy_min = 1e9f, cy_max = -1e9f, cp_min = 1e9f, cp_max = -1e9f;

    for (int i = 0; i < 160; ++i) {   // 2 秒
      roll += roll_rate * DT;
      if (roll > 180.0f) roll -= 360.0f;
      const float c = std::cos(roll * kRad), s = std::sin(roll * kRad);

      // 相机滚转 φ 时，世界向量在相机系里表现为 R(-φ)·v
      const float yaw_cam = c * true_yaw + s * true_pitch;
      const float pit_cam = -s * true_yaw + c * true_pitch;

      yaw_min = std::fmin(yaw_min, yaw_cam); yaw_max = std::fmax(yaw_max, yaw_cam);
      p_min = std::fmin(p_min, pit_cam);     p_max = std::fmax(p_max, pit_cam);

      // 用测到的 roll 做反向旋转，恢复世界向量
      const float cyaw = c * yaw_cam - s * pit_cam;
      const float cpit = s * yaw_cam + c * pit_cam;
      cy_min = std::fmin(cy_min, cyaw); cy_max = std::fmax(cy_max, cyaw);
      cp_min = std::fmin(cp_min, cpit); cp_max = std::fmax(cp_max, cpit);
    }
    printf("%-12.0f  yaw %6.1f~%6.1f°, p %5.1f~%5.1f  yaw %5.2f~%5.2f, p %5.2f~%5.2f\n",
           roll_rate, yaw_min, yaw_max, p_min, p_max, cy_min, cy_max, cp_min, cp_max);
  }

  // =====================================================================
  printf("\n\n==== 场景2：目标往返 0.6m @5m 距离、匀速、慢速（三角波）====\n");
  printf("角幅度 ±%.1f°，往返周期 3 秒 → 峰值角速度 %.1f°/s\n",
         std::atan(0.3f / 5.0f) * kDeg, (std::atan(0.3f / 5.0f) * kDeg) * 4.0f / 3.0f);
  printf("叠加测量噪声 ±0.5°（模拟检测抖动）\n\n");
  printf("%-26s %11s %11s %11s\n", "方案", "最大误差", "平均误差", "输出抖动");
  printf("----------------------------------------------------------------\n");

  {
    const float amp = std::atan(0.3f / 5.0f) * kDeg;   // ≈3.43°
    const float period = 3.0f;
    const float steps_per_sec = FPS;
    Rng rng(7);

    // 三种方案的累积误差
    float k_max = 0, k_sum = 0, e_max = 0, e_sum = 0, r_max = 0, r_sum = 0;
    float k_jit = 0, e_jit = 0, r_jit = 0, k_prev = 0, e_prev = 0, r_prev = 0;
    int n = 0;

    // 简单 alpha-beta（近似卡尔曼）用 2 状态
    float kx = 0, kv = 0;
    const float alpha = 0.85f, beta = 0.02f;   // 高 alpha = 紧跟
    OneEuro oe(1.5f, 0.9f, 1.0f);
    bool first = true;

    for (int i = 0; i < (int)(steps_per_sec * 9); ++i) {
      const float t = i * DT;
      // 三角波：匀速往返
      const float ph = std::fmod(t / period, 1.0f);
      const float tri = (ph < 0.25f) ? (ph * 4.0f)
                      : (ph < 0.75f) ? (2.0f - ph * 4.0f)
                                     : (ph * 4.0f - 4.0f);
      const float truth = amp * tri;
      const float meas = truth + rng.uni() * 0.5f;

      if (first) { kx = meas; oe.reset(meas); k_prev = e_prev = r_prev = meas; first = false; }

      // alpha-beta
      const float kpred = kx + kv * DT;
      const float kres = meas - kpred;
      kx = kpred + alpha * kres;
      kv = kv + (beta / DT) * kres;

      // OneEuro
      const float ef = oe.filter(meas, DT);

      if (i > 20) {
        const float ek = std::fabs(kx - truth);
        const float ee = std::fabs(ef - truth);
        const float er = std::fabs(meas - truth);
        k_max = std::fmax(k_max, ek); k_sum += ek;
        e_max = std::fmax(e_max, ee); e_sum += ee;
        r_max = std::fmax(r_max, er); r_sum += er;
        k_jit += std::fabs(kx - k_prev); e_jit += std::fabs(ef - e_prev); r_jit += std::fabs(meas - r_prev);
        ++n;
      }
      k_prev = kx; e_prev = ef; r_prev = meas;
    }
    printf("%-26s %10.2f° %10.2f° %10.3f°/帧\n", "卡尔曼/alpha-beta", k_max, k_sum / n, k_jit / n);
    printf("%-26s %10.2f° %10.2f° %10.3f°/帧\n", "One Euro Filter", e_max, e_sum / n, e_jit / n);
    printf("%-26s %10.2f° %10.2f° %10.3f°/帧\n", "直通测量", r_max, r_sum / n, r_jit / n);
  }

  // =====================================================================
  printf("\n\n==== 场景3：转向点（三角波尖端）的表现 ====\n");
  printf("匀速往返在端点速度瞬间反向，检查三种方案在端点后被甩开多少\n\n");
  printf("%-26s %14s\n", "方案", "端点后最大误差");
  printf("--------------------------------------\n");
  {
    const float amp = std::atan(0.3f / 5.0f) * kDeg;
    const float period = 3.0f;

    auto run_case = [&](int mode) {
      float kx = 0, kv = 0, k_max = 0;
      const float alpha = 0.85f, beta = 0.02f;
      OneEuro oe(1.5f, 0.9f, 1.0f);
      bool first = true, in_turn = false;
      int turn_cnt = 0;
      for (int i = 0; i < (int)(FPS * 9); ++i) {
        const float t = i * DT;
        const float ph = std::fmod(t / period, 1.0f);
        const float tri = (ph < 0.25f) ? (ph * 4.0f)
                        : (ph < 0.75f) ? (2.0f - ph * 4.0f)
                                       : (ph * 4.0f - 4.0f);
        const float truth = amp * tri;
        const float meas = truth;   // 无噪声，只看模型失配
        if (first) { kx = meas; oe.reset(meas); first = false; }
        const float kpred = kx + kv * DT;
        const float kres = meas - kpred;
        kx = kpred + alpha * kres;
        kv = kv + (beta / DT) * kres;
        const float ef = oe.filter(meas, DT);

        const float out = (mode == 0) ? kx : (mode == 1) ? ef : meas;
        // 端点：ph 接近 0.25 或 0.75
        const bool turn = (std::fabs(ph - 0.25f) < 0.03f) || (std::fabs(ph - 0.75f) < 0.03f);
        if (turn) { in_turn = true; turn_cnt = 0; }
        if (in_turn && ++turn_cnt < 25) {
          k_max = std::fmax(k_max, std::fabs(out - truth));
        } else if (in_turn) {
          in_turn = false;
        }
      }
      return k_max;
    };
    printf("%-26s %13.2f°\n", "卡尔曼/alpha-beta", run_case(0));
    printf("%-26s %13.2f°\n", "One Euro Filter", run_case(1));
    printf("%-26s %13.2f°\n", "直通测量", run_case(2));
  }

  return 0;
}
