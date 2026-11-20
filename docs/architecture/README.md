# 架构图说明

这四张 PNG **由脚本生成**，不要直接改图片；改图请改 `tools/draw_architecture.py` 后重新生成：

```bash
python tools/draw_architecture.py
```

脚本只依赖 Pillow，字体使用 Windows 自带的微软雅黑（`C:/Windows/Fonts/msyh.ttc`），因此中文不会缺字。

| 图 | 内容 |
|---|---|
| `10_总览与内存布局.png` | 传感器 → VI → VB 池 → 缓存映射 → 4× 抽样 → 检测/跟踪 → 角度/瞄点 → 网页与串口；附关键实测数字与三条设计约束 |
| `11_主线程逐帧流水线.png` | 主线程 14 步（取帧、FrameTiming、预热过滤、一次性调参、映射、抽样、检测、筛选+卡尔曼、角度+瞄点、画框、日志、发布、帧交接），右侧为耗时预算、开关、失败分支与日志核对方法 |
| `12_帧的借还与所有权.png` | VI 帧从 VB 池借出与归还、pending 槽（深度 1）如何覆盖旧帧、失败与退出路径 |
| `13_线程同步与检测算法.png` | 五个线程各自读写什么、用哪把锁；检测算法 7 步与 confirmed/coast/lost 三态机 |

## 一帧的旅程（数据流图，用于审核文档）

| 文件 | 说明 |
|---|---|
| `licheeRV_nano_updated.drawio` | 以原图（`C:\Users\admin\Downloads\licheeRV_nano.drawio`）为基础的最小改动版：补了帧时间戳、检测/跟踪措辞、1.4 MB 帧大小，新增"串口线程"框，并按语义上色 |
| `licheeRV_nano_colored.png` | 由上面这份 drawio 渲染出的高清 PNG（2504×1904），插入中期审核文档作为"图 2-1"（整页横向） |
| `tools/render_drawio_png.py` | 渲染脚本：读 `.drawio` 的几何/样式/连线，正交折线 + 自适应字号输出 PNG |

**改图流程**：在 draw.io 里改 `licheeRV_nano_updated.drawio` → 存盘 → 运行

```bash
python tools/render_drawio_png.py docs/architecture/licheeRV_nano_updated.drawio docs/architecture/licheeRV_nano_colored.png
```

→ 再让文档重新生成（`docs/MIDTERM_REVIEW.md` 里引用这张 PNG）。

## 记录与诊断（2026-11 新增）

| 文件 | 内容 |
|---|---|
| `licheeRV_nano_with_record.drawio` / `.png` | **五张同尺寸线程卡片并排**的数据流图：<br>• 卡片统一 360×660，从左到右按数据流排：**① 主线程 → ④ 串口 → ⑤ 记录器 → ② 编码 → ③ HTTP**（编号仍是代码里的线程序号，不重排）；<br>• 卡片外：左侧「硬件」（GC4653 → VB 池）与「电控 STM32」，下方是「运行状态与退出归因（diag：不开线程）」横条；<br>• 左上角图例：`一张卡片 = 一个线程；框 = 步骤或文件`，以及线程数（全关 1 / 典型 5 / 4 网页连满 9）；<br>• 记录器卡片里两个入口框标了「→ 由主线程调用 on_frame()」「→ 由串口线程调用 on_mcu()」，表示那段代码跑在**调用者**线程里，真正的线程是本卡的「write/fdatasync」；<br>• 主线程卡片末尾有「④b OSD（可选）」说明：编码前把电控状态烧进 Y 平面 |
| `flight_timeline.drawio` / `.png` | **一次飞行的时间线**（流程图）：通电 → 启动 → 待机预录 → `STATE=FLIGHT` 触发落盘 → `STATE=STOP` 收尾 → 退出留痕，右侧标出每一步产出哪个文件 |
| `row_life.drawio` / `.png` | **一行数据的一生**（最常被问的问题）：电控帧到达 → 加锁写共享快照 → 取帧 → 立刻读快照（带 `mcu_rx_us`/`age`）→ 组一行文本 → 入队 → 写线程落盘。图上标了真实时间戳（852982879 / 852990741 / 7862 µs），并说明"写盘不加时刻""一行只有两个时刻" |
| `thread_dataflow.png` | **线程间数据流与共享结构**（Pillow，`tools/gen_thread_dataflow.py`）：五条共享通道（电控快照 / 待编码帧 / 最新 JPEG / 记录器缓冲 / 计数器），每条都标了谁写、谁读、用什么锁、多大、是整体赋值还是 swap 指针 |
| `frame_timeline.png` | **两个数据流的时间轴**（Pillow 生成，`tools/gen_frame_timeline.py`）：100Hz 电控帧刻度 vs 80Hz 相机帧刻度，画出一帧落在两帧之间时的两个候选（绿=取左边最近即过去、橙=更近但在未来不可用），以及差几毫秒写进 `mcu_age_ms` || `tools/gen_flight_timeline.py` | 上图的生成脚本（改文案/加步骤改这里，重跑即可） |

> 主线程色带里那个「④b OSD」小框是**可选步骤**：`osd_enable: true` 时，主线程在把帧交给
> 编码线程之前，把电控状态（STATE/陀螺/姿态/目标角 + 数据年龄）烧进 Y 平面左上角。
> 因为烧在像素里，网页预览、录像、抓图三处都能看到，编码线程丢帧也不会让画面和数据错位。
> 实现见 `src/osd.h`（8×8 点阵字库，由 `tools/gen_osd_font.py` 生成）。

渲染命令同上：

```bash
python tools/render_drawio_png.py docs/architecture/licheeRV_nano_with_record.drawio docs/architecture/licheeRV_nano_with_record.png
python tools/render_drawio_png.py docs/architecture/flight_timeline.drawio docs/architecture/flight_timeline.png
```

> `licheeRV_nano_with_record.drawio` 是在原图基础上增补的（本次用脚本插入新区域，之后直接在 draw.io 里改即可，不再从旧图重生成），
> 后续直接在 draw.io 里改这个文件即可；`flight_timeline` 则是脚本生成，改文案请改脚本。


## 历史说明

早期这组图用 draw.io 绘制并导出 PNG。随着代码迭代（多目标跟踪、串口、时间戳、瞄准点偏移、自启），图和代码脱节，而且导出图里大量中文变成方框（字体缺失）。现已改为脚本生成，源文件即脚本；旧 `.drawio` 已移除，需要时可在 git 历史中找回。
