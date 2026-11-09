// 目标跟踪与角度换算：多候选筛选 + 加权评分 + 卡尔曼（角度域）
//
// 设计参考 kalapaopao/LicheeRV-Nano-GC4653 与 HERO dart_vision 的做法：
//   1. 每帧对全部候选跑一次加权评分（质量分 + 离预测位置的距离），选一个
//   2. 用 2 状态（角度, 角速度）卡尔曼平滑，yaw/pitch 各一个滤波器
//   3. 用 confirm/lost 帧数做双层确认，避免单帧误检驱动云台
//   4. 输出角度 / 角速度 / 距离，供串口发给电控
//
// 注意：坐标约定与「相机安装方向」强相关，见 CameraCalib 的注释。
#ifndef GREEN_TARGET_TRACK_H_
#define GREEN_TARGET_TRACK_H_

#include "green_detect.h"

namespace gt {

// ---------------------------------------------------------------------------
// 相机标定参数（由 YAML 提供；标定后填入真实值）
// ---------------------------------------------------------------------------
struct CameraCalib {
  // 针孔模型内参（像素）。1280x720 的典型初值见 config.yaml 注释。
  float fx = 1024.0f;
  float fy = 1024.0f;
  float cx = 640.0f;
  float cy = 360.0f;

  // 光轴补偿：装配后微调，单位度。正值表示需要把结果往负方向修正。
  float optical_yaw_deg = 0.0f;
  float optical_pitch_deg = 0.0f;

  // 方向翻转：不同安装方式（正装/倒装/镜像）用这两个开关适配，无需改代码。
  bool invert_yaw = false;
  bool invert_pitch = false;

  // 瞄准点向上偏移系数（抄自 HERO dart_vision_26 的 TARGET_UP_OFFSET_HEIGHT_FACTOR=1.5）：
  //   瞄准点 = 目标中心上方 (factor × 目标像素直径)。
  //   物理含义 = (装甲板中心到引导灯中心的距离) / (灯直径)，所以与距离无关、自动按框高缩放。
  //   0 = 直接打灯中心；1.5 = 打灯中心上方 1.5 个灯直径处（装甲板）。
  float aim_up_offset_factor = 0.0f;

  bool has_intrinsics() const { return fx > 1.0f && fy > 1.0f; }

  // 检测网格坐标 → 角度（度）。
  //   gx, gy : green_detect 输出的网格坐标（0..w-1, 0..h-1）
  //   S      : 抽点步长（把网格坐标还原成全分辨率像素用）
  //   yaw    : 正 = 目标在光轴右侧
  //   pitch  : 正 = 目标在光轴上方（图像 y 向下，故内部取负）
  void gridToAngleDeg(float gx, float gy, int S, float* yaw, float* pitch) const;
};

// ---------------------------------------------------------------------------
// 卡尔曼参数（角度域，单位度 / 度每帧）
// ---------------------------------------------------------------------------
struct KalmanCfg {
  float process_noise = 50.0f;    // 过程噪声（越大越信任新测量）
  float measure_noise = 0.001f;   // 测量噪声（越大越平滑、越滞后）
  float init_pos_var = 10.0f;     // 初始位置方差
  float init_vel_var = 2.0f;      // 初始速度方差
  float dt_default = 0.0125f;     // 默认帧间隔（80fps）
  float dt_min = 0.002f;          // dt 裁剪下限
  float dt_max = 0.2f;            // dt 裁剪上限
};

// ---------------------------------------------------------------------------
// 多目标筛选参数
// ---------------------------------------------------------------------------
struct SelectCfg {
  int confirm_frames = 5;       // 连续命中多少帧才算「已确认」
  int lost_frames = 10;         // 连续未命中多少帧才算「丢失」
  float conf_weight = 0.5f;     // 评分里质量分的权重（其余给距离分）
  float max_aspect = 1.5f;      // 长宽比上限（长边/短边）
  int max_candidates = 8;       // 单帧最多保留的候选数
  float max_dist_deg = 60.0f;   // 距离归一化尺度（度）
};

enum class TargetState { kLost, kCoasting, kConfirmed };

// ---------------------------------------------------------------------------
// 对外输出（一帧的结果）
// ---------------------------------------------------------------------------
struct TargetOut {
  bool detected = false;         // 本帧是否对外报「有目标」
  TargetState state = TargetState::kLost;

  float yaw_deg = 0.0f;          // 目标相对光轴的水平角（正=右）——已加瞄准点偏移
  float pitch_deg = 0.0f;        // 目标相对光轴的俯仰角（正=上）——已加瞄准点偏移
  float v_yaw_dps = 0.0f;        // 水平角速度（度/秒），给电控做前馈
  float v_pitch_dps = 0.0f;      // 俯仰角速度（度/秒）

  // 引导灯中心的角度（未加瞄准点偏移），只用于联调对照日志
  float light_yaw_deg = 0.0f;
  float light_pitch_deg = 0.0f;

  float distance_m = 0.0f;       // 单目测距（需 target_diameter_m 已知）
  bool distance_valid = false;

  unsigned long seq = 0;         // 递增序号（每次状态更新 +1）
  int misses = 0;                // 连续未命中帧数
  int confirm_count = 0;         // 连续命中帧数
  float age_ms = 0.0f;           // 距最近一次有效测量的毫秒数（数据新鲜度）
  int candidates = 0;            // 本帧候选数量（调试用）

  // 供画框用（检测网格坐标，未经 S 放大）
  bool has_box = false;
  float gx = 0.0f;
  float gy = 0.0f;
  float gr = 0.0f;
};

// ---------------------------------------------------------------------------
// 2 状态卡尔曼（位置 + 速度），匀速模型
// ---------------------------------------------------------------------------
class Kalman1D {
 public:
  void init(float pos, const KalmanCfg& c);
  void predict(float dt, float q);
  void update(float z, float r);
  float pos() const { return x_[0]; }
  float vel() const { return x_[1]; }

 private:
  float x_[2] = {0.0f, 0.0f};
  float P_[2][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}};
};

// ---------------------------------------------------------------------------
// 目标跟踪器：多候选筛选 + 卡尔曼 + 确认/丢失状态机
// ---------------------------------------------------------------------------
class TargetTracker {
 public:
  void configure(const CameraCalib& calib, const KalmanCfg& kf, const SelectCfg& sel,
                 float target_diameter_m);

  // cands : 本帧候选（green_detect::detectMulti 的结果）
  // n     : 候选数量
  // S     : 抽点步长
  // dt    : 距上一帧的秒数
  // now_ms: 单调时钟毫秒（用于计算数据年龄）
  const TargetOut& update(const greendet::Blob* cands, int n, int grid_S, float dt,
                          double now_ms);

  const TargetOut& output() const { return out_; }
  void reset();

 private:
  CameraCalib calib_;
  KalmanCfg kf_;
  SelectCfg sel_;
  float target_diameter_m_ = 0.05f;

  bool started_ = false;      // 卡尔曼是否已初始化
  bool have_last_measure_ = false;
  double last_measure_ms_ = 0.0;

  Kalman1D kf_yaw_;
  Kalman1D kf_pitch_;

  TargetOut out_;
};

}  // namespace gt

#endif  // GREEN_TARGET_TRACK_H_
