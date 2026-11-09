# 代码导读：从哪里开始看

> 这份文档假设你**没读过这个项目的代码**，按顺序读下去，每一步都告诉你
> 「看哪个文件、看哪几行、为什么看这里、看懂什么算过关」。
>
> ⚠️ 文中的**行号基于早期版本**，后续迭代（多目标跟踪、串口、时间戳、瞄准点偏移）
> 会让行号漂移；**以函数名/小节名为准**，行号只做快速定位参考。

---

## 第 0 步：先搞清楚该读什么、不该读什么

先看规模，避免一头扎进不该读的地方：

| 文件 | 行数 | 是谁的代码 | 要读吗 |
|---|---|---|---|
| `src/camera.cpp` | **493** | 我们写的 | ★★★ **必读，核心** |
| `src/green_detect.cpp` | **335** | 我们写的 | ★★★ **必读，算法** |
| `src/green_detect.h` | **113** | 我们写的 | ★★★ **必读，参数都在这里** |
| `src/avi_mjpeg.h` | 160 | 我们写的 | ★ 用到再看 |
| `src/sensor/gc4653_cmos.c` | 1015 | 项目自带驱动 | ✗ 不要读，只看 `cmos_set_image_mode` 一个函数 |
| `src/sensor/gc4653_sensor_ctl.c` | 537 | 项目自带驱动 | ✗ 不要读，只有寄存器表 |
| `src/common/sample_common_*.c` | **约 6200** | 厂商 CVITEK 示例 | ✗ **绝对不要通读**，只在报错时查 |

**结论：真正要读的只有约 1100 行。** 剩下 7000 行是厂商 sample 代码，
它们的作用只是「把摄像头的硬件细节包装成几个函数调用」。

### 你要先接受的 5 个概念

| 概念 | 一句话解释 |
|---|---|
| **VI** | 从摄像头传感器取帧的那层驱动。给你一块内存里的像素数据 |
| **VENC** | 硬件 JPEG 编码器。你把帧丢给它，它吐出 JPEG。**不占 CPU** |
| **VB 池** | 一块预先分配的图像内存，大小按分辨率算。尺寸算错会 `Out of memory` |
| **物理地址 / 虚拟地址** | 摄像头写进物理内存；CPU 要通过 `mmap` 才能读。**未缓存的 mmap 极慢** |
| **NV12** | 一种 YUV 排列：先是整张亮度图（Y），后面跟着缩小一半的色度（UV 交织） |

---

## 第 1 步：先看数据长什么样（不看代码）

在读任何代码前，先建立「一帧图像在内存里是什么」的直觉。

打开 `src/green_detect.h` 第 22~50 行，这就是全部可调参数：

```cpp
struct Config {
  int u_max = 124;      // U < u_max 且 V < v_max ⇒ 绿色方向（128 为中性）
  int v_max = 124;
  int y_min = 100;      // 发光亮度下限
  int sat_min = 30;     // 饱和度下限 = sqrt((U-128)^2 + (V-128)^2)
  int y_fallback = 60;  // 目标变暗时的回退阈值
  int sat_fallback = 25;
  float area_min = 2.0f;      // 连通域面积（检测网格上的像素数）
  float area_max = 400.0f;
  float fill_min = 0.50f;     // 面积 / 外接矩形面积
  float aspect_max = 2.20f;   // 宽高比上限（圆 ≈ 1.0）
  float radius_max = 12.0f;
  float gate_px = 28.0f;      // 跟踪门控半径
  float coast_frames = 20;    // 跟丢后允许预测的帧数
};
```

**记住三个数**：Y 是亮度（0~255），U/V 是色度（128 = 无色）。
绿色 = 偏「负 U 负 V」方向。所以判据是 `U<124 && V<124`。

**过关标准**：你能解释「为什么不用 RGB 而用 YUV」。

---

## 第 2 步：看算法主流程（不纠缠细节）

打开 `src/green_detect.cpp`，只看三个函数的**开头和结尾**：

```
第 21 行   void Detector::buildMask(...)     ← 造黑白掩膜
第 71 行   void Detector::morphOpen(...)     ← 去噪
第 122 行  bool Detector::detectFrame(...)   ← 主流程（重点）
第 272 行  const TrackState& Tracker::process(...)  ← 跟踪状态机
```

### 2.1 `detectFrame` 的骨架（122~267 行）

这是算法的「主函数」，把它压缩成伪代码：

```c
bool detectFrame(...) {
  // ① 造掩膜：逐像素判断"是不是绿色且亮" → mask_ 数组里 1/0
  buildMask(y, u, v, w, h, false, ..., &n);
  if (n == 0) {
      // ② 一个绿点都没有 → 如果之前在跟踪，用更松的阈值在门控附近再试一次
      if (allow_fallback && gate_cx) buildMask(..., true, ...);
  }
  if (n == 0) return false;        // 还是没有 → 这一帧没目标

  morphOpen(w, h);                 // ③ 3x3 开运算，去掉孤立噪点

  // ④ 连通域：把所有相邻的 1 聚成一块块，每块算面积/外接框/重心
  for (每个未访问的 1 像素) {
      flood_fill(...);             // 泛洪，算出 area/minx/maxx/中心
      // ⑤ 形状过滤：面积太大太小、太扁、不填充 → 丢掉
      // ⑥ 评分：亮度 + 饱和度 − 距离，留最高分
  }
  return found;
}
```

**过关标准**：你能说出「为什么先 buildMask 再 morphOpen 再连通域」这个顺序。

### 2.2 泛洪填充的写法（147~197 行）——这是重点技巧

```cpp
static thread_local std::vector<int> visited;   // ① 复用，不每帧 new
visited.assign(mask_.size(), 0);
stack_.clear();

stack_.push_back(sidx);              // ② 显式栈，不用递归
visited[sidx] = 1;
while (!stack_.empty()) {
    const int cur = stack_.back();
    stack_.pop_back();
    const int cy_ = cur / w;         // ③ 一维下标 → 二维坐标
    const int cx_ = cur % w;
    ++area;
    sumx += cx_;  sumy += cy_;       // ④ 累加求重心
    // 更新 bbox...
    for (int dy = -1; dy <= 1; ++dy)      // ⑤ 8 邻域
      for (int dx = -1; dx <= 1; ++dx) {
          ...
          if (mask_[nidx] && !visited[nidx]) {
              visited[nidx] = 1;
              stack_.push_back(nidx);
          }
      }
}
```

**三个值得学的点**：

1. **`thread_local` + `assign`**：连通域要一块 `visited` 数组和一个栈。如果每帧
   `new`，80fps 就是每秒 80 次堆分配，会碎片化内存。这里复用。
2. **显式栈代替递归**：递归 flood fill 遇到大色块会爆栈（嵌入式栈通常只有几十 KB）。
3. **一维数组模拟二维**：`cur / w` 和 `cur % w` 取出坐标。比 `vector<vector<>>` 快得多，
   因为内存连续。

**过关标准**：你能说清「为什么不用递归」「为什么 visited 要 thread_local」。

---

## 第 3 步：看跟踪状态机（这是"稳定"的关键）

`green_detect.cpp` 第 272~338 行，`Tracker::process`。先看它的三个状态：

```
kDetected  ──命中──> 正常输出坐标
kCoast     ──跟丢但没超时──> 靠速度预测继续输出（画面不闪）
kLost      ──跟丢超时──> 停止输出
```

核心逻辑（简化）：

```cpp
// ① 如果正在跟踪，用「上一帧位置 + 速度」预测这一帧目标在哪
if (tracking) {
    gate_cx = st_.cx + st_.vx;      // 预测位置
    gate_r  = gate_px * (1 + 0.3 * misses);   // 跟丢越久，搜索圈越大
}

// ② 在预测位置附近找目标（带门控）
bool hit = detectFrame(..., pgate_cx, pgate_cy, gate_r, ...);

// ③ 跳变保护：突然跑到很远 → 认为是别的东西，不算命中
if (hit && tracking && dist(blob, st_) > max_jump_px) hit = false;

// ④ 三态转移
if (hit) {
    st_.vx = (1-vel_smooth) * st_.vx + vel_smooth * (blob.cx - st_.cx);  // 平滑速度
    st_.status = kDetected;
    st_.misses = 0;
} else if (tracking) {
    if (++st_.misses <= coast_frames) {
        st_.status = kCoast;
        st_.cx += st_.vx;  st_.cy += st_.vy;      // 继续预测
        st_.vx *= coast_decay;                     // 速度逐渐衰减
    } else {
        st_.status = kLost;                        // 彻底放弃
    }
}
```

**为什么需要 Coast？** 目标被手挡一下、或者闪一下没检测到，如果直接报"丢失"，
下游（云台、显示）会抖动。Coast 让它在 20 帧内继续按预测位置输出。

**过关标准**：你能解释 `gate_px`、`max_jump_px`、`coast_frames` 各自防的是什么问题。

| 参数 | 防的问题 |
|---|---|
| `gate_px` | 满屏都是绿色时，只认预测位置附近的，避免跳到别的地方 |
| `max_jump_px` | 突然出现一个远处的绿点，不要当成"目标瞬移了" |
| `coast_frames` | 短暂遮挡不要立刻报丢失 |

---

## 第 4 步：看主程序（`camera.cpp`）——按执行顺序

这是最长的文件（493 行），但**按执行顺序读就清楚了**。先看目录：

| 行号 | 内容 | 读吗 |
|---|---|---|
| 1~75 | include + 全局变量 | 扫一眼，知道有哪些共享状态 |
| 40~52 | `map_cached()` 地址映射缓存 | ★★ 读，这是性能关键 |
| 86~131 | `http_client()` 单个浏览器连接 | 可跳，用到再看 |
| 133~157 | `http_server()` 分配连接槽 | 可跳 |
| 158~191 | `encoder_worker()` 编码线程 | ★★ 读，理解消费者 |
| 192~318 | `main()` 前半：解析参数 + 初始化硬件 | ★★ 读，理解启动顺序 |
| **319~497** | **主循环** | ★★★ **最核心，逐行读** |

### 4.1 先看全局变量（1~75 行）——它们告诉你有哪些部件

```cpp
static std::atomic<int> running(1);        // 所有线程的退出标志

static std::mutex jpeg_mutex;              // 保护"最新 JPEG"
static std::vector<unsigned char> latest;  // 最新一帧 JPEG（给网页）
static unsigned long generation = 0;       // 版本号：网页靠它判断"有没有新帧"

static std::mutex input_mutex;             // 保护"待编码帧"
static VIDEO_FRAME_INFO_S pending{};       // 待编码的帧（深度 1 的槽）
static bool has_pending = false;

static AviMjpegWriter avi;                 // 录像器
static greendet::Tracker g_tracker;        // 检测器+跟踪器
```

**看懂这 10 行，就看懂了整个架构**：两个互斥锁 + 两个共享数据，
就是「主线程 → 编码线程 → HTTP 线程」的全部通信。

### 4.2 主循环：一帧的完整旅程（319~497 行）

```cpp
while (running) {
    VIDEO_FRAME_INFO_S frame{};
    // ① 取一帧。1000ms 超时就 continue（不会卡死）
    if (CVI_VI_GetChnFrame(0, 0, &frame, 1000)) { ++timeouts; continue; }

    // ② 前 4 秒丢弃（ISP 还没稳定）
    if (t - start < 4) { CVI_VI_ReleaseChnFrame(0,0,&frame); continue; }

    // ③ 拿到物理地址 → 映射成虚拟地址才能读像素
    if (frame.stVFrame.pu8VirAddr[0] == nullptr) {
        frame.stVFrame.pu8VirAddr[0] = map_cached(PhyAddr[0], len0);
        frame.stVFrame.pu8VirAddr[1] = map_cached(PhyAddr[1], len1);
    }

    // ④ 抽点下采样：1280x720 → 320x180（16 倍数据量削减）
    //    每个 4x4 块取 ns×ns 个点里的最大值
    (循环填 gy / gu / gv 三个数组)

    // ⑤ 调检测器
    const greendet::TrackState& st = g_tracker.process(gy, gu, gv, w, h);

    // ⑥ 如果检测到，把框画进 Y 平面（注意：是画进图像本身！）
    if (g_display_on && st.status != kLost) {
        (在 Y 平面上写 255 画 3px 白框)
        CVI_SYS_IonFlushCache(...);   // ← 关键：缓存写回 DRAM
    }

    // ⑦ 把帧交给编码线程（或直接归还）
    if (g_display_on) {
        pending = frame;  has_pending = true;   // 交给消费者
    } else {
        CVI_VI_ReleaseChnFrame(0, 0, &frame);   // 没人要，直接还
    }
}
```

**这一段的精髓在第 ④ 和第 ⑥ 步**，下面单独讲。

### 4.3 第 ④ 步：为什么必须抽点（这是性能的核心）

1280×720 = **921,600 像素**。每个像素要读 Y + U + V。
如果全图检测，光读像素就是 90 万次内存访问，在 1GHz CPU 上要几十毫秒——
远超 12.5ms 的帧预算。

所以先抽点：**每 4×4 个像素只保留一些代表点**，缩小到 320×180 = 57,600 点，
数据量降到 **1/16**，检测就快了。

代码（397~426 行）：

```cpp
static unsigned char gy[320*180], gu[160*90], gv[160*90];   // 静态数组，不重复分配
const int S = (W >= 2000) ? 8 : 4;      // 步长：720P 用 4
const int w = W / S, h = H / S;         // 320 x 180

const int ns = g_sample_steps;          // 每块采样点数（1~4，YAML 可调）
const int sstep = (ns > 1) ? (S / ns) : S;

for (int yy = 0; yy < h; ++yy) {
  for (int xx = 0; xx < w; ++xx) {
      unsigned m = 0;
      for (int sy = 0; sy < ns; ++sy) {          // 块内纵向采 ns 点
          const unsigned ry = yy*S + sy*sstep;
          if (ry >= y_rows) break;               // ★ 边界保护（曾因缺它崩溃）
          const unsigned char* row = Yp + ry * ystride;
          for (int sx = 0; sx < ns; ++sx) {
              const unsigned cx = xx*S + sx*sstep;
              if (cx >= W) break;                // ★ 边界保护
              const unsigned v = row[cx];
              if (v > m) m = v;                  // ★ 取最大值，不是平均值
          }
      }
      gy[yy*w + xx] = (unsigned char)m;
  }
}
```

**三个关键决策，每个都有代价**：

| 决策 | 为什么 | 代价 |
|---|---|---|
| **取 `max` 不取平均** | 发光目标是很小的亮斑。平均会把它稀释到看不见 | 计算量略大 |
| `ns=1`（默认） | 只取 1 个点 → 采样 4.5ms，能保 80fps | **覆盖率仅 1/16，小目标可能漏检** |
| `ns=2/4` | 覆盖率提升到 1/4、1 | 耗时 8.8ms / 26ms → **帧率掉到 47/20fps** |

这就是文档里说的"**满帧与检测灵敏度二选一**"的来源。

### 4.4 第 ⑥ 步：画框为什么要 flush 缓存

```cpp
unsigned char* Yw = (unsigned char*)Yp;
for (int t = 0; t < 3; ++t) {           // 3 像素粗的线
    // 画上、下两条横线，左、右两条竖线
    for (int x = x0; x <= x1; ++x) Yw[ya*ystride + x] = 255;   // 255 = 纯白
}
// ★ 关键：把 CPU 缓存里的改动写回物理内存
CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[0], (CVI_VOID*)Yw, stride*H);
```

**为什么必须 flush**：我们用的是**缓存映射**（`CVI_SYS_MmapCache`）。
CPU 写的数据先进 L1/L2 缓存，而 **VENC 硬件编码器直接读物理内存**。
不 flush 的话，硬件读到的是旧数据 → 网页上看不到框。

> 实测：忘了 flush 的表现是「日志说画了框，画面没框」。

---

## 第 5 步：看编码线程（消费者，158~191 行）

很短，但有一个值得学的模式：

```cpp
static void encoder_worker() {
  while (running) {
      VIDEO_FRAME_INFO_S frame{};  bool got = false;
      {
          std::lock_guard<std::mutex> g(input_mutex);
          if (has_pending) { frame = pending; has_pending = false; got = true; }
      }                                            // ← 锁立刻释放
      if (!got) { usleep(1000); continue; }        // 没帧就睡 1ms

      CVI_VENC_StartRecvFrame(0, &receive);        // 告诉硬件：来 1 帧
      CVI_VENC_SendFrame(0, &frame, 1000);         // 送进去
      CVI_VENC_GetStream(0, &stream, 1000);        // 取回 JPEG

      if (recording_enabled) avi.write(...);       // ① 写录像
      { std::lock_guard<std::mutex> g(jpeg_mutex);
        latest.swap(jpg);  ++generation; }         // ② 给网页

      CVI_VI_ReleaseChnFrame(0, 0, &frame);        // ③ 归还帧缓冲
  }
}
```

**两个设计点**：

1. **`swap` 而不是拷贝**：`latest.swap(jpg)` 只交换指针，O(1)。
   如果用赋值，每帧要拷贝几十 KB。
2. **一份 JPEG 三处用**：录像、网页、抓图都用这同一份字节流，
   **不做第二次编码**。这是省 CPU 的关键。

**注意 ③ 的顺序**：`ReleaseChnFrame` 必须在编码完成之后，因为 VENC 还在读这块内存。

---

## 第 6 步：一次完整的数据流追踪

建议你**自己画一遍**这张图，画对了就说明读懂了：

```
传感器 GC4653
   │ 硬件写 DMA 到物理内存（VB 池里的一块）
   ▼
VI 驱动 ──CVI_VI_GetChnFrame()──> 你拿到 VIDEO_FRAME_INFO_S
   │                                里面有 PhyAddr[]（物理地址）
   │                                VirAddr[] 是空的！
   ▼
map_cached(PhyAddr) ──CVI_SYS_MmapCache()──> 拿到可读的虚拟地址
   │  （第 1 次会真映射，之后命中 g_va[16] 缓存）
   ▼
抽点下采样 ──> gy[320*180] / gu[160*90] / gv[160*90]
   │
   ▼
g_tracker.process() ──> buildMask → morphOpen → 连通域 → 形状过滤
   │                    → 门控 → 跳变保护 → 三态机
   ▼
TrackState { status, cx, cy, vx, vy, blob.radius }
   │
   ├──> printf("GREEN_TARGET ...")            日志
   └──> 在 Y 平面画白框 + IonFlushCache       烧进图像
              │
              ▼
        pending 槽（深度 1）
              │ input_mutex
              ▼
        encoder_worker ──CVI_VENC_SendFrame()──> 硬件 JPEG
              │
              ├──> latest (jpeg_mutex + generation) ──> http_client ──> 浏览器
              └──> AviMjpegWriter ──> recordings/*.avi
```

---

## 第 7 步：自己动手验证理解

读完不要只看，做这四件事：

### 7.1 改一个参数看效果

```sh
cd /code/fly_LicheeRV
sed -i 's/^detect_sample_steps: .*/detect_sample_steps: 2/' config.yaml
./start.sh 20
# 看日志 DETECT 从 5.3ms 变成 ~8.8ms，host_fps 从 80 掉到 47
```

### 7.2 关掉检测，确认取流本身不吃力

```sh
sed -i 's/^detect_enable: .*/detect_enable: false/' config.yaml
./start.sh 20
# host_fps 应该还是 80，DETECT 行消失
```

### 7.3 制造一次 Coast

把绿色目标在摄像头前快速晃过再拿开，看日志：

```
GREEN_TARGET status=detected ...    ← 命中
GREEN_TARGET status=coast ...       ← 短暂预测（目标已经离开）
（coast_frames 用完）
（不再输出）
```

### 7.4 改日志看内部状态

在 `camera.cpp` 第 434 行后面加一行，把算法内部数据打出来：

```cpp
const greendet::TrackState& st = g_tracker.process(gy, gu, gv, w, h);
// 加这一行，看"这一帧有多少像素通过了颜色阈值"
if (g_frame_idx % 30 == 0)
    printf("DEBUG mask_px=%d status=%d misses=%d\n",
           g_tracker.lastMaskPixels(), (int)st.status, st.misses);
```

`mask_px` 是最有用的调试量：
- 一直是 0 → 颜色阈值不对（或者画面没目标）
- 几千 → 阈值太松，满屏都被当绿色
- 几十 → 正常（一个小灯）

---

## 附：推荐阅读顺序总结

```
① src/green_detect.h        22~50 行   只看 Config（参数全貌）
② src/green_detect.cpp     122~267 行  detectFrame 主流程（跳过细节看结构）
③ src/green_detect.cpp     147~197 行  泛洪填充（学技巧）
④ src/green_detect.cpp     272~338 行  Tracker 三态机（学"稳定"）
⑤ src/camera.cpp            40~52 行   map_cached（性能关键）
⑥ src/camera.cpp            60~75 行   全局变量（看懂架构）
⑦ src/camera.cpp           319~497 行  主循环（核心，逐行）
⑧ src/camera.cpp           158~191 行  encoder_worker（消费者模式）
⑨ src/avi_mjpeg.h                      用到再看
```

**不要读**：`src/common/*.c`、`src/sensor/*.c`（除 `cmos_set_image_mode`）。

---

## 附：如果你想知道「为什么代码长成这样」

几个关键决策的来龙去脉，都在 `docs/` 里：

| 疑问 | 去哪看 |
|---|---|
| 为什么不用 OpenCV / NPU | `README.md` 开头的功能说明 + 本文第 2 步 |
| 为什么检测不单独开线程 | 本文第 4.2 节 + `architecture/11_主线程逐帧流水线.png` |
| 为什么 mmap 要用 Cache 版本 | `camera.cpp` 里 `map_cached()` 上方注释：未缓存时 223ms/帧 |
| 为什么传感器驱动要自己编译 | `HANDOFF_ISSUE_720p_80fps.md` |
| 整体架构图 | `architecture/10_总览与内存布局.png` |
| 串口协议与自检 | `UART_PROTOCOL.md` |
