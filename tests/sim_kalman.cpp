// 压力测试：现有卡尔曼参数在两种真实场景下的表现
//   场景A：目标左右往返（第二形态）—— 检查滞后与转向点过冲
//   场景B：相机自身在转（飞镖/云台），目标静止 —— 检查 v_yaw 是否被污染
// 编译：g++ -std=c++11 -O1 -I src sim_kalman.cpp src/target_track.cpp src/green_detect.cpp -o sim
#include <cmath>
#include <cstdio>

#include "green_detect.h"
#include "target_track.h"

namespace {
constexpr float kPi = 3.14159265358979f;
const int S = 4;
const float FPS = 80.0f;
const float DT = 1.0f / FPS;

// 用「网格坐标」喂给 tracker：由角度反算像素
float angle_to_gridx(float yaw_deg, const gt::CameraCalib& c) {
  // yaw = atan((px-cx)/fx) * 180/pi  →  px = cx + fx*tan(yaw)
  const float rad = yaw_deg * kPi / 180.0f;
  const float px = c.cx + c.fx * std::tan(rad);
  return px / S;
}
}  // namespace

int main() {
  gt::CameraCalib calib;
  calib.fx = 1024.0f;
  calib.fy = 1024.0f;
  calib.cx = 640.0f;
  calib.cy = 360.0f;

  gt::KalmanCfg kf;   // 用 config.yaml 里的默认值
  kf.process_noise = 50.0f;
  kf.measure_noise = 0.001f;

  gt::SelectCfg sel;
  sel.confirm_frames = 1;
  sel.lost_frames = 100;
  sel.conf_weight = 0.5f;

  // =====================================================================
  printf("================ 场景A：目标左右往返（第二形态）================\n");
  printf("振幅 ±15°，检查跟踪滞后与转向点表现\n");
  printf("注：amp 已是度，w 是 rad/s，故真实角速度幅值 = amp*w（度/秒）\n\n");
  printf("%-8s %10s %10s %10s %11s %11s\n",
         "频率", "最大误差", "平均误差", "转向点误差", "v_yaw峰值", "真实值");
  printf("--------------------------------------------------------------------\n");

  for (float freq : {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 8.0f}) {
    gt::TargetTracker tr;
    tr.configure(calib, kf, sel, 0.05f);

    const float amp = 15.0f;   // 度
    const float w = 2.0f * kPi * freq;
    float max_err = 0.0f, sum_err = 0.0f, turn_err = 0.0f, vpeak = 0.0f;
    int n = 0;

    const int steps = (int)(FPS * 4);   // 跑 4 秒
    for (int i = 0; i < steps; ++i) {
      const float t = i * DT;
      const float yaw_true = amp * std::sin(w * t);

      greendet::Blob b;
      b.cx = angle_to_gridx(yaw_true, calib);
      b.cy = 90.0f;
      b.radius = 8.0f;
      b.val = 200.0f;
      b.sat = 60.0f;
      b.aspect = 1.0f;
      b.fill = 0.8f;

      tr.update(&b, 1, S, DT, t * 1000.0f);
      const gt::TargetOut& o = tr.output();

      if (i > 20) {   // 跳过初始收敛
        const float err = std::fabs(o.yaw_deg - yaw_true);
        max_err = std::fmax(max_err, err);
        sum_err += err;
        ++n;
        vpeak = std::fmax(vpeak, std::fabs(o.v_yaw_dps));
        // 转向点：cos 过零附近（速度方向反转）
        if (std::fabs(std::cos(w * t)) < 0.12f) {
          turn_err = std::fmax(turn_err, err);
        }
      }
    }

    const float vr = amp * w;   // 度/秒（amp 已是度，无需再乘 180/π）
    printf("%-8.1f %9.2f° %9.2f° %9.2f° %10.1f %10.1f\n",
           freq, max_err, n ? sum_err / n : 0.0f, turn_err, vpeak, vr);
  }

  // =====================================================================
  printf("\n\n========= 场景A2：3Hz 往返下，调参能改善多少 =========\n");
  printf("目标：找出「既跟得上往返、又不抖」的参数组合\n\n");
  printf("%-28s %10s %10s %10s\n", "process/measure 噪声", "最大误差", "平均误差", "v_yaw峰值");
  printf("------------------------------------------------------------------\n");

  struct Tune { float q, r; const char* tag; };
  const Tune tunes[] = {
      {50.0f, 0.001f, "q=50   r=0.001 (当前)"},
      {50.0f, 0.05f, "q=50   r=0.05  (更平滑)"},
      {50.0f, 0.5f, "q=50   r=0.5   (强平滑)"},
      {500.0f, 0.001f, "q=500  r=0.001 (更快)"},
      {5000.0f, 0.001f, "q=5000 r=0.001 (最快)"},
      {5.0f, 0.01f, "q=5    r=0.01  (重平滑)"},
  };

  for (const auto& tn : tunes) {
    gt::KalmanCfg k2 = kf;
    k2.process_noise = tn.q;
    k2.measure_noise = tn.r;
    gt::TargetTracker tr;
    tr.configure(calib, k2, sel, 0.05f);

    const float amp = 15.0f, freq = 3.0f;
    const float w = 2.0f * kPi * freq;
    float max_err = 0.0f, sum_err = 0.0f, vpeak = 0.0f;
    int n = 0;
    const int steps = (int)(FPS * 4);
    for (int i = 0; i < steps; ++i) {
      const float t = i * DT;
      const float yaw_true = amp * std::sin(w * t);
      greendet::Blob b;
      b.cx = angle_to_gridx(yaw_true, calib);
      b.cy = 90.0f; b.radius = 8.0f; b.val = 200.0f;
      b.sat = 60.0f; b.aspect = 1.0f; b.fill = 0.8f;
      tr.update(&b, 1, S, DT, t * 1000.0f);
      const gt::TargetOut& o = tr.output();
      if (i > 20) {
        const float err = std::fabs(o.yaw_deg - yaw_true);
        max_err = std::fmax(max_err, err);
        sum_err += err; ++n;
        vpeak = std::fmax(vpeak, std::fabs(o.v_yaw_dps));
      }
    }
    printf("%-28s %9.2f° %9.2f° %10.1f\n", tn.tag, max_err,
           n ? sum_err / n : 0.0f, vpeak);
  }

  // =====================================================================
  printf("\n\n======= 场景A3：测量带噪声时，卡尔曼到底有没有在平滑 =======\n");
  printf("3Hz 往返 + 每次测量叠加 ±0.5° 随机噪声（模拟检测抖动）\n\n");
  printf("%-28s %10s %10s\n", "配置", "最大误差", "平均误差");
  printf("--------------------------------------------------\n");

  for (float meas_noise : {0.0f, 0.3f, 0.8f}) {
    for (int use_kf = 1; use_kf >= 0; --use_kf) {
      // use_kf=0 时把 measure_noise 推到极大 → 几乎完全忽略测量，等价于纯预测
      // 这里改用「直接把测量当输出」来对比
      gt::KalmanCfg k2 = kf;
      if (!use_kf) { k2.process_noise = 1e5f; k2.measure_noise = 1e-6f; }  // 紧跟测量
      gt::TargetTracker tr;
      tr.configure(calib, k2, sel, 0.05f);

      const float amp = 15.0f, freq = 3.0f;
      const float w = 2.0f * kPi * freq;
      float max_err = 0.0f, sum_err = 0.0f;
      int n = 0;
      unsigned seed = 12345;
      const int steps = (int)(FPS * 4);
      for (int i = 0; i < steps; ++i) {
        const float t = i * DT;
        const float yaw_true = amp * std::sin(w * t);
        // 简单线性同余随机数
        seed = seed * 1103515245u + 12345u;
        const float rnd = ((seed >> 16) & 0x7FFF) / 16384.0f - 1.0f;  // [-1,1]
        const float measured = yaw_true + rnd * meas_noise;

        greendet::Blob b;
        b.cx = angle_to_gridx(measured, calib);
        b.cy = 90.0f; b.radius = 8.0f; b.val = 200.0f;
        b.sat = 60.0f; b.aspect = 1.0f; b.fill = 0.8f;
        tr.update(&b, 1, S, DT, t * 1000.0f);
        const gt::TargetOut& o = tr.output();
        if (i > 20) {
          const float err = std::fabs(o.yaw_deg - yaw_true);
          max_err = std::fmax(max_err, err);
          sum_err += err; ++n;
        }
      }
      char tag[80];
      snprintf(tag, sizeof(tag), "噪声±%.1f° %s", meas_noise,
               use_kf ? "卡尔曼(当前)" : "近乎直通测量");
      printf("%-28s %9.2f° %9.2f°\n", tag, max_err, n ? sum_err / n : 0.0f);
    }
  }

  // =====================================================================
  printf("\n\n================ 场景B：相机自身在转，目标静止 ================\n");
  printf("目标世界角度固定 10°；相机以不同角速度转向目标\n");
  printf("检查 v_yaw 输出的是「目标运动」还是「相对运动」\n\n");
  printf("%-14s %14s %14s %14s\n",
         "相机角速度", "画面里测得", "输出v_yaw", "输出yaw(末)");
  printf("------------------------------------------------------------------\n");

  for (float cam_rate : {0.0f, 20.0f, 50.0f, 100.0f, -50.0f}) {
    gt::TargetTracker tr;
    tr.configure(calib, kf, sel, 0.05f);

    const float target_world_yaw = 10.0f;   // 目标在世界坐标里固定 10°
    float cam_yaw = 0.0f;                    // 相机当前朝向
    float last_v = 0.0f, last_yaw = 0.0f;

    const int steps = (int)(FPS * 1.0f);   // 跑 1 秒
    for (int i = 0; i < steps; ++i) {
      const float t = i * DT;
      cam_yaw += cam_rate * DT;            // 相机转动
      // 画面里看到的角度 = 目标世界角 - 相机朝向角
      const float observed = target_world_yaw - cam_yaw;

      // 相机转太多，目标跑出画面 → 停止喂
      if (std::fabs(observed) > 30.0f) break;

      greendet::Blob b;
      b.cx = angle_to_gridx(observed, calib);
      b.cy = 90.0f;
      b.radius = 8.0f;
      b.val = 200.0f;
      b.sat = 60.0f;
      b.aspect = 1.0f;
      b.fill = 0.8f;

      tr.update(&b, 1, S, DT, t * 1000.0f);
      last_v = tr.output().v_yaw_dps;
      last_yaw = tr.output().yaw_deg;
    }

    printf("%-14.0f %13.2f° %13.1f°/s %13.2f°\n",
           cam_rate, target_world_yaw - cam_yaw, last_v, last_yaw);
  }

  printf("\n说明：目标在世界坐标里完全静止（10°不动），\n");
  printf("      但相机转动时 v_yaw 输出的是「相对角速度」= -相机转速。\n");
  return 0;
}
