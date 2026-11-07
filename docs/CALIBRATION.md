# 相机标定与参数填写指南

> 这份文档解决一件事：**`config.yaml` 里那几个标定参数该填什么、怎么来、怎么验证对不对。**
>
> 当前 `config.yaml` 里填的是**占位值**（从同型号 1280×720 相机估算），
> 不标定也能跑，但角度会有系统偏差——不能用于闭环控制。

---

## 一、当前占位值是什么，为什么不能直接用

```yaml
cam_fx: 1023.2      # 焦距（水平方向，像素）
cam_fy: 1022.2      # 焦距（垂直方向，像素）
cam_cx: 643.8       # 光心 x（像素）
cam_cy: 382.8       # 光心 y（像素）
```

这四个数是**针孔模型内参**。它们决定「像素 → 角度」的换算：

```
angle_x = atan((px - cx) / fx)
```

**为什么不能直接用**：
- 这些值来自同型号相机的另一颗镜头（同一批次但不保证相同）
- 镜头装配有公差，`cx/cy` 每个模块都不同
- 如果 `cx` 实际是 640 而填了 643.8，画面中心就会出现 0.2° 的固定偏差

**误差的量级**：`fx` 偏差 10% → 边缘角度偏差约 3°。
对 3 米外的目标，3° 意味着 **15cm 的瞄准偏差**。所以必须标定。

---

## 二、标定方法（棋盘格，约 30 分钟）

### 2.1 准备

| 项 | 要求 |
|---|---|
| 棋盘格 | 打印一张，贴在硬板上（要绝对平整）。推荐 9×6 内角点，方格边长 25mm |
| 拍摄 | 用**本项目的相机**（分辨率必须是 1280×720），距棋盘 0.5~3 米 |
| 数量 | 15~25 张，覆盖：正对、左倾、右倾、上仰、下俯、四角 |
| 关键 | **棋盘要占画面 1/3 以上**；四角必须拍到（角落畸变最大） |

### 2.2 拍照

```sh
# 板端：用抓图接口连续拍，手动移动棋盘
cd /code/fly_LicheeRV
./start.sh 300 &          # 启动，保持 5 分钟

# 电脑上循环抓图（每按一次回车存一张）
while ($true) {
  $ts = Get-Date -Format "yyyyMMdd_HHmmss"
  curl.exe -s -o "calib_$ts.jpg" http://10.133.124.1:8081/snapshot.jpg
  Write-Host "saved calib_$ts.jpg  (回车继续, Ctrl+C 结束)"
  Read-Host
}
```

⚠️ 标定时**把 `detect_enable` 设 false**，否则画框会破坏棋盘角点检测：

```sh
sed -i 's/^detect_enable: .*/detect_enable: false/' config.yaml
```

### 2.3 计算内参

电脑上（需要 Python + OpenCV）：

```python
# save as calibrate.py
import cv2, numpy as np, glob, json, sys

PATTERN = (9, 6)        # 内角点数（横, 竖）—— 按你的棋盘改
SQUARE_MM = 25.0        # 方格实际边长（毫米）—— 按你的打印尺寸改

objp = np.zeros((PATTERN[0]*PATTERN[1], 3), np.float32)
objp[:, :2] = np.mgrid[0:PATTERN[0], 0:PATTERN[1]].T.reshape(-1, 2) * SQUARE_MM

objpoints, imgpoints, used = [], [], []
for f in sorted(glob.glob('calib_*.jpg')):
    img = cv2.imread(f)
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    ok, corners = cv2.findChessboardCorners(gray, PATTERN, None)
    if not ok:
        print('skip (no corners):', f); continue
    corners = cv2.cornerSubPix(gray, corners, (11,11), (-1,-1),
        (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001))
    objpoints.append(objp); imgpoints.append(corners); used.append(f)
    print('ok:', f)

if len(used) < 8:
    sys.exit('有效图像太少(%d)，至少 8 张' % len(used))

h, w = cv2.imread(used[0]).shape[:2]
rms, K, dist, _, _ = cv2.calibrateCamera(objpoints, imgpoints, (w, h), None, None)

print('\n有效图像: %d / 尺寸: %dx%d' % (len(used), w, h))
print('重投影误差 RMS = %.3f px   (越小越好，<0.5 合格)' % rms)
print('\n--- 填入 config.yaml ---')
print('cam_fx: %.1f' % K[0][0])
print('cam_fy: %.1f' % K[1][1])
print('cam_cx: %.1f' % K[0][2])
print('cam_cy: %.1f' % K[1][2])
print('\n畸变系数 (k1,k2,p1,p2,k3):', dist.ravel())
json.dump({'rms': rms, 'K': K.tolist(), 'dist': dist.tolist(),
           'images': len(used)}, open('calib_result.json','w'), indent=2)
```

运行：

```powershell
python calibrate.py
```

**判断合格**：
- `RMS < 0.5 px` → 好
- `RMS 0.5~1.0` → 可用（可能有 0.5° 误差）
- `RMS > 1.0` → 重拍（棋盘不平、模糊、覆盖不够）

### 2.4 填进 YAML

把脚本输出的 4 个数填进 `config.yaml` 和 `config_boot.yaml`：

```yaml
cam_fx: 1015.3      # ← 用你的实际值
cam_fy: 1014.8
cam_cx: 638.2
cam_cy: 371.5
```

**两个文件都要改**（手动运行用 `config.yaml`，开机自启用 `config_boot.yaml`）。

---

## 三、验证标定对不对

### 3.1 无目标时的合理性检查

```sh
cd /code/fly_LicheeRV
sh start.sh 30
grep 'TRACK calib' /tmp/fly_licheerv_capture.log
```

应该看到你填的值被正确读入：

```
TRACK calib fx=1015.30 fy=1014.80 cx=638.20 cy=371.50 ...
```

### 3.2 用已知角度验证（关键一步）

**方法**：把绿色目标放在**精确已知角度**的位置，看输出对不对。

```
准备：一根 1 米长的直尺/绳子，一端固定在相机镜头中心正下方
      在 1 米处标记：左 0.2m、0.5m、1.0m；右同样
      这些位置对应的角度 = atan(0.2/1) = 11.3°
                        atan(0.5/1) = 26.6°
                        atan(1.0/1) = 45.0°
```

打开串口看输出（或看日志里的 `GREEN_TARGET yaw=`）：

| 目标位置（1m 处） | 理论 yaw | 实测 yaw | 偏差 |
|---|---|---|---|
| 正中 | 0.0° | ? | 应 <0.5° |
| 左 0.2m | -11.3° | ? | |
| 左 0.5m | -26.6° | ? | |
| 右 0.2m | +11.3° | ? | |
| 右 0.5m | +26.6° | ? | |

**如果所有点都有相同的偏差** → 调 `optical_yaw_deg` 抵消（见第四节）。

**如果偏差随角度变大** → 说明 `fx` 不准，或畸变没校正（本项目目前**不做畸变校正**，见第六节）。

### 3.3 如果符号反了

`yaw` 应该是「右为正」。如果实测左边是正、
右边是负，说明相机装反了，改：

```yaml
cam_invert_yaw: true
```

`pitch` 应该是「上为正」。如果上下颠倒：

```yaml
cam_invert_pitch: true
```

---

## 四、光轴补偿怎么调

**用途**：把「镜头光轴」和「机械安装基准」的固定偏差补掉。

**场景**：相机装在云台上，云台零位时镜头其实偏了 1.5°。

**调法**（在标定内参之后做）：

```
① 把目标放在「机械零位」方向（云台 0° 指向的位置）
② 看实测 yaw：如果显示 +1.5°，说明视觉认为目标在右边 1.5°
   但实际它在机械零位 → 需要减掉这 1.5°
③ 设 optical_yaw_deg: 1.5
④ 重启，再测，应该显示 ≈0.0°
```

公式：

```
最终 yaw = atan((px - cx)/fx) × 180/π − optical_yaw_deg
```

**每次改 `optical_*` 都要重启程序**（目前不支持热更新）。

---

## 五、目标直径怎么测（单目测距用）

```yaml
target_diameter_m: 0.05     # 当前占位值 5cm，必须实测
```

**测法**：

1. 用游标卡尺量**发光圆形区域**的直径（不是整个灯壳）
2. 换算成米：50mm → `0.05`
3. 填进 YAML

**这个值只影响距离，不影响角度**。如果不需要距离，可以不管它
（`distance_valid` 会是 true 但值不准）。

**精度要求**：距离误差 ≈ 直径误差。直径差 10% → 距离差 10%。

**怎么验证距离准不准**：

```
把目标放在已知距离（卷尺量）1.0m / 2.0m / 3.0m
看串口里的 dist 字段
偏差应该在 ±10% 以内（受直径测量精度限制）
```

---

## 六、已知限制（如实说明）

| 限制 | 影响 | 现状 |
|---|---|---|
| **不做畸变校正** | 画面边缘角度偏大（本机 k1≈−0.45，边缘误差可达 2~3°） | 未实现。近距离/居中时可忽略；大角度时需要补 |
| **单目测距依赖已知直径** | 换目标就要改 `target_diameter_m` | 设计如此 |
| 标定值随对焦变化 | 重新对焦后应重标 | 未处理 |
| 温度漂移 | 长时间运行光轴可能微移 | 未验证 |

**如果需要畸变校正**，可以加一步（用标定得到的 `dist` 系数）：

```cpp
// 在 gridToAngleDeg 之前先做去畸变
// 需要把 OpenCV 的 5 参数模型移植成纯 C++（约 20 行）
```

目前没做，因为：
- 检测网格是抽点得到的（320×180），本身已经降采样
- 目标通常在画面中间区域
- 加畸变校正会让每帧多一次坐标变换

**如果实测发现边缘误差 >1°，就需要补上。**

---

## 七、快速检查清单

标定完成后，逐项打勾：

- [ ] `RMS < 0.5 px`
- [ ] `cam_fx/fy` 在 900~1200 范围内（1280×720 的典型值）
- [ ] `cam_cx` 在 600~680，`cam_cy` 在 340~400（应该在中心附近，但不一定正好 640/360）
- [ ] `config.yaml` **和** `config_boot.yaml` 都改了
- [ ] 重启后 `TRACK calib` 日志显示新值
- [ ] 正中目标 yaw ≈ 0（±0.5°）
- [ ] 左右符号正确（右为正）
- [ ] 上下符号正确（上为正）
- [ ] 已知角度点偏差 <1°
- [ ] 距离实测偏差 <10%（如需测距）

---

## 八、参数速查表

| YAML 键 | 含义 | 单位 | 怎么来 |
|---|---|---|---|
| `cam_fx` | 水平焦距 | 像素 | 棋盘格标定 |
| `cam_fy` | 垂直焦距 | 像素 | 棋盘格标定 |
| `cam_cx` | 光心 x | 像素 | 棋盘格标定 |
| `cam_cy` | 光心 y | 像素 | 棋盘格标定 |
| `optical_yaw_deg` | 水平光轴补偿 | 度 | 用机械零位微调 |
| `optical_pitch_deg` | 俯仰光轴补偿 | 度 | 用机械零位微调 |
| `cam_invert_yaw` | 水平符号翻转 | bool | 装反时改 |
| `cam_invert_pitch` | 俯仰符号翻转 | bool | 装反时改 |
| `target_diameter_m` | 目标实际直径 | 米 | 卡尺量 |
