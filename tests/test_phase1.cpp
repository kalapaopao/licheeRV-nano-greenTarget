// 原生（host）单元测试：不依赖板端 SDK，只验证纯算法部分。
// 编译：g++ -std=c++11 -O1 -I src test_phase1.cpp src/target_track.cpp src/green_detect.cpp -o t
#include <cmath>
#include <cstdio>
#include <cstring>

#include "frame_timing.h"
#include "green_detect.h"
#include "target_track.h"
#include "uart_out.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const char* what, const char* detail = "") {
  if (cond) {
    ++g_pass;
    printf("  PASS  %s\n", what);
  } else {
    ++g_fail;
    printf("  FAIL  %s   %s\n", what, detail);
  }
}

static void check_near(float got, float want, float tol, const char* what) {
  char buf[160];
  snprintf(buf, sizeof(buf), "got=%.4f want=%.4f tol=%.4f", got, want, tol);
  check(std::fabs(got - want) <= tol, what, buf);
}

// ---------------------------------------------------------------------------
// 1. 角度换算
// ---------------------------------------------------------------------------
static void test_angles() {
  printf("\n[1] 像素/网格 → 角度\n");
  gt::CameraCalib c;
  c.fx = 1000.0f;
  c.fy = 1000.0f;
  c.cx = 640.0f;
  c.cy = 360.0f;
  const int S = 4;   // 抽点步长

  float yaw = 0.0f, pitch = 0.0f;

  // 正中心（网格坐标 = 全分辨率/S）
  c.gridToAngleDeg(160.0f, 90.0f, S, &yaw, &pitch);
  check_near(yaw, 0.0f, 0.001f, "中心 yaw = 0");
  check_near(pitch, 0.0f, 0.001f, "中心 pitch = 0");

  // 向右 100 像素：atan(100/1000) = 5.7106°
  c.gridToAngleDeg(160.0f + 100.0f / S, 90.0f, S, &yaw, &pitch);
  check_near(yaw, 5.7106f, 0.01f, "右 100px → yaw=+5.71°");
  check_near(pitch, 0.0f, 0.001f, "水平移动 pitch 不变");

  // 向下 100 像素：pitch 应为 -5.7106°（图像 y 向下 = 俯仰向下）
  c.gridToAngleDeg(160.0f, 90.0f + 100.0f / S, S, &yaw, &pitch);
  check_near(pitch, -5.7106f, 0.01f, "下 100px → pitch=-5.71°");

  // 光轴补偿：yaw 减 2°
  c.optical_yaw_deg = 2.0f;
  c.gridToAngleDeg(160.0f + 100.0f / S, 90.0f, S, &yaw, &pitch);
  check_near(yaw, 3.7106f, 0.01f, "光轴补偿 yaw-2°");
  c.optical_yaw_deg = 0.0f;

  // 翻转
  c.invert_yaw = true;
  c.gridToAngleDeg(160.0f + 100.0f / S, 90.0f, S, &yaw, &pitch);
  check_near(yaw, -5.7106f, 0.01f, "invert_yaw 生效");
  c.invert_yaw = false;

  // 未标定（fx=0）时应退化，不崩
  gt::CameraCalib bad;
  bad.fx = 0.0f;
  bad.fy = 0.0f;
  bad.gridToAngleDeg(160.0f, 90.0f, S, &yaw, &pitch);
  check(std::isfinite(yaw) && std::isfinite(pitch), "未标定不崩且输出有限值");
}

// ---------------------------------------------------------------------------
// 2. 卡尔曼：跟踪匀速运动
// ---------------------------------------------------------------------------
static void test_kalman() {
  printf("\n[2] 卡尔曼（匀速目标）\n");
  gt::KalmanCfg kf;
  kf.process_noise = 50.0f;
  kf.measure_noise = 0.001f;
  kf.init_pos_var = 10.0f;
  kf.init_vel_var = 2.0f;

  gt::TargetTracker tr;
  gt::CameraCalib c;
  c.fx = 1000.0f;
  c.fy = 1000.0f;
  c.cx = 640.0f;
  c.cy = 360.0f;
  gt::SelectCfg sel;
  sel.confirm_frames = 3;
  sel.lost_frames = 3;
  tr.configure(c, kf, sel, 0.05f);

  // 目标以每帧 +20 像素（= +20/S 网格）向右匀速移动
  const int S = 4;
  const float dt = 0.0125f;
  greendet::Blob b;
  b.radius = 8.0f;
  b.val = 200.0f;
  b.sat = 60.0f;
  b.aspect = 1.0f;
  b.fill = 0.8f;

  double now_ms = 0.0;
  float last_yaw = 0.0f;
  for (int i = 0; i < 30; ++i) {
    b.cx = 160.0f + (20.0f * i) / S;
    b.cy = 90.0f;
    tr.update(&b, 1, S, dt, now_ms);
    now_ms += dt * 1000.0;
    if (i == 29) {
      last_yaw = tr.output().yaw_deg;
    }
  }

  const gt::TargetOut& o = tr.output();
  check(o.state == gt::TargetState::kConfirmed, "30 帧后状态 = confirmed");
  check(o.detected, "detected = true");
  check(o.confirm_count == 30, "confirm_count = 30");

  // 第 30 帧理论 yaw：atan((20*29)/1000) = atan(0.58) = 30.11°
  check_near(last_yaw, 30.11f, 1.5f, "末帧 yaw ≈ 30.1°");

  // 角速度理论值：20px/帧 @80fps = 1600px/s；d(atan(x/1000))/dt
  //   在 x≈0 处 ≈ 1600/1000 = 1.6 rad/s = 91.7°/s
  //   在 x≈580px 处 ≈ (1600/1000)/(1+0.58²) = 1.198 rad/s = 68.6°/s
  // 卡尔曼有滞后，允许较大区间
  char buf[160];
  snprintf(buf, sizeof(buf), "v_yaw=%.1f°/s", o.v_yaw_dps);
  check(o.v_yaw_dps > 40.0f && o.v_yaw_dps < 130.0f, "v_yaw 量级合理(≈70~90°/s)", buf);

  // 单目测距：radius=8 网格 → 像素直径 2*8*4=64px，f=1000，D=0.05 → z=1000*0.05/64=0.781m
  // 但输出的是「斜距」= sqrt(x²+y²+z²)，末帧 yaw=30.1° → x=tan(30.1°)*z=0.453
  //   → 斜距 = sqrt(0.453² + 0.781²) = 0.903m
  snprintf(buf, sizeof(buf), "dist=%.3f", o.distance_m);
  check(o.distance_valid, "距离有效");
  check(o.distance_m > 0.85f && o.distance_m < 0.95f, "斜距 ≈ 0.90m（含横向修正）", buf);
}

// ---------------------------------------------------------------------------
// 3. 状态机：确认 → coast → lost
// ---------------------------------------------------------------------------
static void test_state_machine() {
  printf("\n[3] 确认 / coast / lost 状态机\n");
  gt::KalmanCfg kf;
  gt::CameraCalib c;
  c.fx = 1000.0f;
  c.fy = 1000.0f;
  c.cx = 640.0f;
  c.cy = 360.0f;
  gt::SelectCfg sel;
  sel.confirm_frames = 3;
  sel.lost_frames = 4;
  gt::TargetTracker tr;
  tr.configure(c, kf, sel, 0.05f);

  const int S = 4;
  greendet::Blob b;
  b.cx = 160.0f; b.cy = 90.0f; b.radius = 8.0f;
  b.val = 200.0f; b.sat = 60.0f; b.aspect = 1.0f; b.fill = 0.8f;

  double t = 0.0;
  // 第 1 帧：命中但未确认
  tr.update(&b, 1, S, 0.0125f, t); t += 12.5;
  check(tr.output().state == gt::TargetState::kCoasting, "第1帧 = coasting（未达确认帧数）");
  check(!tr.output().detected, "第1帧不对外报 detected");
  // 第 3 帧：确认
  tr.update(&b, 1, S, 0.0125f, t); t += 12.5;
  tr.update(&b, 1, S, 0.0125f, t); t += 12.5;
  check(tr.output().state == gt::TargetState::kConfirmed, "第3帧 = confirmed");
  check(tr.output().detected, "confirmed 时报 detected");

  // 连续 4 帧无候选 → coast 仍报 detected
  for (int i = 0; i < 4; ++i) {
    tr.update(nullptr, 0, S, 0.0125f, t); t += 12.5;
  }
  check(tr.output().state == gt::TargetState::kCoasting, "4 帧丢失内 = coasting");
  check(tr.output().detected, "coast 期间仍报 detected");

  // 第 5 帧丢失 → lost
  tr.update(nullptr, 0, S, 0.0125f, t); t += 12.5;
  check(tr.output().state == gt::TargetState::kLost, "超过 lost_frames = lost");
  check(!tr.output().detected, "lost 时不报 detected");

  // 数据年龄应持续增长
  check(tr.output().age_ms > 50.0f, "age_ms 随时间增长");
}

// ---------------------------------------------------------------------------
// 4. 多目标筛选：靠「离预测位置近」保持锁定
// ---------------------------------------------------------------------------
static void test_multi_target() {
  printf("\n[4] 多目标：锁定离预测最近的那个\n");
  gt::KalmanCfg kf;
  gt::CameraCalib c;
  c.fx = 1000.0f; c.fy = 1000.0f; c.cx = 640.0f; c.cy = 360.0f;
  gt::SelectCfg sel;
  sel.confirm_frames = 1;
  sel.lost_frames = 5;
  sel.conf_weight = 0.3f;      // 更偏重「离预测近」
  gt::TargetTracker tr;
  tr.configure(c, kf, sel, 0.05f);

  const int S = 4;
  double t = 0.0;

  // 目标 A 在左边，目标 B 在右边，两者质量相同
  greendet::Blob cands[2];
  for (int i = 0; i < 2; ++i) {
    cands[i].radius = 8.0f;
    cands[i].val = 200.0f;
    cands[i].sat = 60.0f;
    cands[i].aspect = 1.0f;
    cands[i].fill = 0.8f;
  }
  cands[0].cx = 100.0f;   // 左
  cands[1].cx = 220.0f;   // 右

  // 第一次喂「左」，锁定左边
  tr.update(cands, 2, S, 0.0125f, t); t += 12.5;
  const float yaw_left = tr.output().yaw_deg;
  check(yaw_left < 0.0f, "首帧选中左侧目标（yaw<0）");

  // 让右边目标的『质量』远高于左边，验证权重可调
  cands[1].val = 255.0f;
  cands[1].sat = 90.0f;
  tr.update(cands, 2, S, 0.0125f, t); t += 12.5;
  const float yaw2 = tr.output().yaw_deg;
  char buf[160];
  snprintf(buf, sizeof(buf), "yaw1=%.2f yaw2=%.2f", yaw_left, yaw2);
  // conf_weight=0.3 时质量权重低，仍应偏向预测位置（左侧）
  check(yaw2 < 0.0f, "低置信权重下仍跟住左侧（不被高质量目标抢走）", buf);

  // 把 conf_weight 调到 1.0（只看质量），应切换到右侧高质量目标
  gt::SelectCfg sel2 = sel;
  sel2.conf_weight = 1.0f;
  gt::TargetTracker tr2;
  tr2.configure(c, kf, sel2, 0.05f);
  double t2 = 0.0;
  tr2.update(cands, 2, S, 0.0125f, t2); t2 += 12.5;
  const float y1 = tr2.output().yaw_deg;
  tr2.update(cands, 2, S, 0.0125f, t2);
  const float y2 = tr2.output().yaw_deg;
  snprintf(buf, sizeof(buf), "y1=%.2f y2=%.2f", y1, y2);
  check(y2 > 0.0f, "conf_weight=1.0 时切换到高质量（右侧）目标", buf);
}

// ---------------------------------------------------------------------------
// 5. 长宽比过滤
// ---------------------------------------------------------------------------
static void test_aspect_filter() {
  printf("\n[5] 长宽比过滤\n");
  gt::KalmanCfg kf;
  gt::CameraCalib c;
  c.fx = 1000.0f; c.fy = 1000.0f; c.cx = 640.0f; c.cy = 360.0f;
  gt::SelectCfg sel;
  sel.max_aspect = 1.5f;
  sel.confirm_frames = 1;
  gt::TargetTracker tr;
  tr.configure(c, kf, sel, 0.05f);

  greendet::Blob b;
  b.cx = 160.0f; b.cy = 90.0f; b.radius = 8.0f;
  b.val = 200.0f; b.sat = 60.0f; b.fill = 0.8f;

  b.aspect = 3.0f;   // 长条，应被淘汰
  tr.update(&b, 1, 4, 0.0125f, 0.0);
  check(tr.output().state == gt::TargetState::kLost, "aspect=3.0 被过滤");

  gt::TargetTracker tr2;
  tr2.configure(c, kf, sel, 0.05f);
  b.aspect = 1.1f;   // 接近圆，应通过
  tr2.update(&b, 1, 4, 0.0125f, 0.0);
  check(tr2.output().state != gt::TargetState::kLost, "aspect=1.1 通过");
}

// ---------------------------------------------------------------------------
// 6. 帧时间戳与板端年龄（frame_timing.h）
//    这些数字不会自己保证实时性，但必须算对，否则 5 秒日志里的延迟全是错的。
// ---------------------------------------------------------------------------
static void test_frame_timing() {
  printf("\n[6] 帧时间戳 / dt / 板端年龄\n");

  // 6.1 单调时钟：连续取值不倒退，且在前进
  {
    gt::MonoUs prev = gt::monotonic_us();
    bool nondecreasing = true;
    bool advanced = false;
    for (int i = 0; i < 1000; ++i) {
      const gt::MonoUs now_us = gt::monotonic_us();
      if (now_us < prev) {
        nondecreasing = false;
      }
      if (now_us > prev) {
        advanced = true;
      }
      prev = now_us;
    }
    check(nondecreasing, "monotonic_us 连续 1000 次调用不倒退");
    check(advanced, "monotonic_us 会前进（不是常量）");
    char buf[96];
    snprintf(buf, sizeof(buf), "prev=%llu us", (unsigned long long)prev);
    check(prev > 1000000ULL, "monotonic_us 单位是微秒（>1e6 表示已运行超过 1 秒）", buf);
  }

  // 6.2 采集时刻差 → dt
  check_near(gt::capture_dt_s(1000000ULL, 987500ULL, 0.0125f), 0.0125f, 1e-6f,
             "采集差 12500us → dt=0.0125s（80fps）");
  check_near(gt::capture_dt_s(2000000ULL, 1975000ULL, 0.0125f), 0.025f, 1e-6f,
             "跨帧采集差 25000us → dt=0.025s（detect_stride=2）");
  check_near(gt::capture_dt_s(1234ULL, 0ULL, 0.0125f), 0.0125f, 1e-6f,
             "首帧无基准 → 退回默认 dt");
  check_near(gt::capture_dt_s(1000ULL, 2000ULL, 0.0125f), 0.0125f, 1e-6f,
             "时间倒退 → 退回默认 dt（不减成天文数字）");
  check_near(gt::capture_dt_s(1000ULL, 1000ULL, 0.0125f), 0.0125f, 1e-6f,
             "两帧时刻相同 → 退回默认 dt");

  // 6.3 板端年龄：无效输入一律 -1，且不发生无符号下溢
  check(gt::frame_age_ms(true, 0ULL, 5000ULL) == -1, "capture=0 → age=-1");
  check(gt::frame_age_ms(false, 1000ULL, 5000ULL) == -1, "无时间戳 → age=-1");
  check(gt::frame_age_ms(true, 5000ULL, 1000ULL) == -1, "now<capture → age=-1（无符号下溢保护）");
  check(gt::frame_age_us(true, 5000ULL, 1000ULL) == -1, "now<capture → age_us=-1");
  check(gt::frame_age_ms(true, 1000000ULL, 1015500ULL) == 15, "15500us → age=15ms");
  check(gt::frame_age_ms(true, 1000000ULL, 1750000ULL) == 750, "750000us → age=750ms");
  check(gt::frame_age_ms(true, 1000000ULL, 1000999ULL) == 0, "999us → age=0ms（向下取整）");

  // 6.4 FrameTiming 的耗时字段
  {
    gt::FrameTiming ft;
    ft.frame_id = 42;
    ft.capture_mono_us = 1000000ULL;
    ft.detect_start_us = 1002000ULL;
    ft.detect_end_us = 1007500ULL;
    check(ft.valid(), "frame_id/capture 都有值 → valid()");
    check(ft.detect_duration_us() == 5500U, "detect_duration_us = 5500us（抽样→结果完成）");
    check(ft.capture_latency_us() == 7500U, "capture_latency_us = 7500us（取帧→结果完成）");

    const gt::FrameTiming empty;
    check(empty.detect_duration_us() == 0U, "未记录耗时 → 0（不是负数/天文数字）");
    check(!empty.valid(), "空时间戳 → valid()=false");

    gt::FrameTiming back = ft;
    back.detect_end_us = 1ULL;   // 时间倒退
    check(back.detect_duration_us() == 0U, "detect 时间倒退 → 0");
    check(back.capture_latency_us() == 0U, "capture_latency 时间倒退 → 0");
  }

  // 6.5 帧号与结果序号是两套独立编号，不能混用
  {
    gt::UartSnapshot snap;
    snap.target.seq = 3;          // 结果序号：跟踪器更新时才 +1
    snap.timing.frame_id = 1000;  // 相机帧号：每取到一帧 +1
    snap.has_timing = true;
    char buf[96];
    snprintf(buf, sizeof(buf), "seq=%lu frame_id=%u", snap.target.seq, snap.timing.frame_id);
    check(snap.target.seq == 3 && snap.timing.frame_id == 1000,
          "UartSnapshot 同时保留 seq 与 frame_id（互不覆盖）", buf);
    check(snap.has_timing, "UartSnapshot.has_timing 独立记录时间戳是否可用");

    // 不带时间戳的发布路径（启动帧）必须保持 has_timing=false
    gt::UartSnapshot boot;
    boot.target.age_ms = -1.0f;
    check(!boot.has_timing && gt::frame_age_ms(boot.has_timing, boot.timing.capture_mono_us,
                                               gt::monotonic_us()) == -1,
          "启动帧无时间戳 → 板端年龄 -1");
  }
}

int main() {
  printf("=== 阶段1 算法单元测试 ===\n");
  test_angles();
  test_kalman();
  test_state_machine();
  test_multi_target();
  test_aspect_filter();
  test_frame_timing();
  printf("\n=== 结果：%d 通过, %d 失败 ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
