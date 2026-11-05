# 问题求助：720P 模式只跑到 33.7fps（原版 82fps），卡在 VI 输出尺寸

> 写给 `camera_36record` 的作者。
> 同一块板、同一时刻、同一传感器，你的二进制 82.5fps，我重新编译的 33.7fps，
> **已定位唯一差异是 VI 通道输出尺寸**，但所有把它改成 1280×720 的尝试都会让颜色变绿。
> 需要你确认为生产版用了哪套配置。

---

## 1. 目标

在 LicheeRV Nano（SG2002 + GC4653）上做一个**单进程**程序：

```
GC4653 (720P_60FPS) → VI → [绿色圆灯检测 + 画框] → VENC(JPEG) → HTTP MJPEG / AVI 录像
```

要求：**1280×720、~80fps、颜色正确、检测框能出现在网页画面上**。

检测部分已单独验证过（纯传统算法，YUV 阈值 + 连通域 + 跟踪，
`track_ms=0.45`，实测开/关检测帧率差 0.14fps，**不是瓶颈**）。

---

## 2. 对照实测（关键证据）

同一块板，连续两次运行，其它条件一致（36MHz 时钟、auto 曝光、compress=1、JPEG Q85）。

| 项目 | **原版 `camera_36record`** | **本次重编译版本** |
|---|---|---|
| 二进制大小 | 594,584 B | 451,848 B |
| **VI geometry** | **`1280x720` stride=1280,1280 lengths=921600,460800** | **`1920x1080` stride=1920,1920 lengths=2073600,1036800** |
| JPEG 输出尺寸 | 1280×720 | 1280×720 |
| **帧率** | **82.47 fps** | **33.7 fps** |
| 颜色（抓图 RGB 均值） | R119.3 / G111.1 / B111.5 ✓ | R136.9 / G136.0 / B136.5 ✓ |
| `DIAG ISP pub fps` | 30.000000 | 30.000000（**相同**） |
| `DIAG configured_compress` | 1 | 1（相同） |
| `DIAG channel_rc` | 0x0 compress=1 | 0x0 compress=1（相同） |
| `sensor_mode` | 720P_60FPS | 720P_60FPS（相同） |
| `stSnsrMode` | — | `1920 1080 60.000000 wdrMode 0` |

原版日志原文：

```
sensor_mode=80 preview=1280x720 quality=85 preview_limiter=none exposure_mode=auto
RECORD80_BLC_CONFIG mode=default offsets=256,256,256,256
DIAG configured_compress=1
DIAG channel_rc=0 compress=1 video=0 depth=2 pipe_rc=0 pipe_compress=1
VENC CreateChn rc=0x0
DIAG ISP pub fps=30.000000 get=0
VI geometry=1280x720 stride=1280,1280 format=19 compress=0 video=0 lengths=921600,460800,0
capture_frames=496 host_fps=82.4667 ... encoded=492 encode_fps=82.461
```

我的版本日志原文：

```
SENSOR_MODE 720P_60FPS size=1280x720 detect=on display=on
POOL_SIZE 1920x1080
stSnsrMode.u16Width 1920 stSnsrMode.u16Height 1080 60.000000 wdrMode 0 pstSnsObj 0x45410
STEP vi_init rc=0
DIAG channel_rc=0 compress=1 video=0 depth=2 pipe_rc=0 pipe_compress=1
VENC CreateChn rc=0x0
DIAG ISP pub fps=30.000000 get=0
VI geometry=1920x1080 stride=1920,1920 format=19 compress=0 video=0 lengths=2073600,1036800,0
capture_frames=543 host_fps=33.8258 ... encoded=542 encode_fps=33.769
```

**结论：除 VI 输出尺寸外，两版日志完全一致。**

---

## 3. 我的编译方式

源码取自本仓库 `test/clock36_safe_20261019/userland/`：

- `sensor/gc4653_cmos.c`、`gc4653_sensor_ctl.c`、`gc4653_cmos_ex.h`、`gc4653_cmos_param.h`
- `common/sample_common_{bin,isp,platform,sensor,sys,vi,vpss}.c` + `sample_comm.h`
- `include/linux/cif_uapi.h`（提供 `CAMPLL_FREQ_36M`）

编译宏：

```
-O2 -ffunction-sections -fdata-sections
-D__CV181X__ -DARCH_CV181X -DARCH_SG200X -DUSE_USER_SEN_DRIVER=1
-DSENSOR_GCORE_GC4653 -DC906 -D_MIDDLEWARE_V2_ -DUSE_TPU_IVE
-march=rv64imafdcv0p7xthead
```

链接：**动态链接板端 `/mnt/system/usr/lib` 的 .so**
（`libsys libvpu libvenc libisp libisp_algo libae libawb libaf libcvi_bin libcvi_bin_isp`）
+ 静态 `libini`（inih，`ini_parse`）
+ `-Wl,--gc-sections`（丢弃未用的 VO 代码）
+ **不链接 `libsns_gc4653`**（改用源码里的用户态驱动）
+ `-static-libstdc++ -static-libgcc`

主程序逻辑与你的 `camera_36record.cpp` 一致：

```cpp
if(SAMPLE_COMM_VI_ParseIni(&ini)) return 3;
ini.enSnsType[0] = GCORE_GC4653_MIPI_720P_60FPS_10BIT;   // 覆盖 ini
CVI_VI_SetDevNum(ini.devNum);
SAMPLE_COMM_VI_IniToViCfg(&ini, &cfg);
SIZE_S size{1280,720};
SAMPLE_PLAT_SYS_INIT(pool_size);
SAMPLE_PLAT_VI_INIT(&cfg);
```

---

## 4. 已定位的四处配置差异

### 4.1 `SAMPLE_COMM_SNS_GetSize()`（`sample_common_sensor.c`）

```c
// 本仓库 test 版：
case GCORE_GC4653_MIPI_720P_60FPS_10BIT:
    /* Experimental local 1080p timing table under the reused mode enum.
     * Align VI buffer geometry and RAW dump with real sensor window. */
    *penSize = PIC_1080P;
    break;

// Sipeed SDK 原版：
case GCORE_GC4653_MIPI_720P_60FPS_10BIT:
    *penSize = PIC_720P;
    break;
```

### 4.2 `SAMPLE_COMM_SNS_GetPicSize()` 的 `PIC_720P`

```c
// 本仓库 test 版（注释写 1280*720，值却是 1080p）：
case PIC_720P:   /* 1280 * 720 */
    pstSize->u32Width  = 1920;
    pstSize->u32Height = 1080;
    break;

// Sipeed SDK 原版：
case PIC_720P:   /* 1280 * 720 */
    pstSize->u32Width  = 1280;
    pstSize->u32Height = 720;
    break;
```

### 4.3 驱动模式表 `gc4653_cmos_param.h`

```c
[GC4653_MODE_1280X720P60] = {
    .name = "1280X720P30",
    .astImg[0].stSnsSize = { .u32Width = 1920, .u32Height = 1080 },
    .astImg[0].stWndRect = { .s32X = 0, .s32Y = 0, .u32Width = 1920, .u32Height = 1080 },
    .f32MaxFps = 60,
    .f32MinFps = 2.75,
},
```

### 4.4 `cmos_set_image_mode()`（本仓库版有、SDK 版没有的分支）

```c
} else if (pstSensorImageMode->f32Fps <= 180) {
    if (pstSnsState->enWDRMode == WDR_MODE_NONE) {
        if (GC4653_RES_IS_720P(w, h))          // (w<=1920 && h<=1080)
            u8SensorImageMode = GC4653_MODE_1280X720P60;
        else
            u8SensorImageMode = GC4653_MODE_2560X1440P30;
    }
}
```

运行日志确认这段生效了：`stSnsrMode 1920 1080 60.000000` → 命中 `fps<=180` 分支 → 选 `GC4653_MODE_1280X720P60`。

---

## 5. 我试过的方案与结果

| # | 配置 | VI 输出 | 帧率 | 颜色 |
|---|---|---|---|---|
| A | 现状：`GetSize=PIC_1080P`（或 `PIC_720P→1920×1080`），VB 池 1920×1080 | 1920×1080 | 33.7 | ✓ R136.9/G136.0/B136.5 |
| B | `PIC_720P→1280×720`，VB 池 1280×720 | 启动失败 | — | `vi_sdk_enable_chn NG, Out of memory` |
| C | `PIC_720P→1280×720`，VB 池 1920×1080 | 1280×720 | **67.0** | ✗ 全绿 R0 / G135 / B0 |
| D | C + `enBayerFormat: BAYER_FORMAT_GR → GB` | 1280×720 | 33.7 | ✗ 偏绿 R39.7 / G150.9 / B34.9 |
| E | 4M_30FPS 模式（`PIC_1440P`，池 2560×1440） | 2560×1440 | 41.3 | ✓ 正确 |

**方案 C 是整个问题的核心线索**：
把 VI 改成 1280×720 后帧率从 33.7 → **67fps**（说明 VI 尺寸确实是帧率杠杆），
但画面变成纯绿 —— 这与"传感器实际输出 1920×1080、VI 裁切/下采样到 1280×720 导致 Bayer 相位错位"完全吻合。

**由此推断**：你生产版的 720P 寄存器表应该是让传感器**直接输出 1280×720 raw**，
而本仓库 `test/` 目录里的驱动是为 RAW 抓取实验改成 1080p timing 的版本。

---

## 6. 需要你回答的问题

1. **生产版 `camera_36record` 里 `GetSize(GCORE_GC4653_MIPI_720P_60FPS_10BIT)` 返回 `PIC_720P` 还是 `PIC_1080P`？**
   同时 `GetPicSize(PIC_720P)` 返回 1280×720 还是 1920×1080？

2. **生产版 `gc4653_linear_720p60_init()` 的寄存器表，输出窗口（`0x0344`–`0x0354` 一带）配的是 1280×720 还是 1920×1080？**
   如果配的是 1280×720，需要哪些寄存器值（或与 `test/` 版的 diff）？
   驱动里有 `saved_window[17]` / `0x0344+i` 的痕迹，看起来就是这组窗口寄存器。

3. **如果传感器输出 1920×1080 而 VI 只要 1280×720，正确的 `enBayerFormat`（VI dev）和 `enBayer`（ISP pub）应该是什么？**
   我试过 `BAYER_FORMAT_GR`（默认）和 `BAYER_FORMAT_GB`，都不对。
   是否存在"VI 裁剪后 Bayer 相位改变、必须改 VI 起始坐标"的做法？

4. **你构建时传给 `SAMPLE_PLAT_SYS_INIT()` 的 `SIZE_S` 是多少？**
   我按它算 VB 池（`COMMON_GetPicBufferSize`，`COMPRESS_MODE_NONE`，8 块）。
   VI=1280×720 且池=1280×720 时报 `Out of memory`，只有池按 1920×1080 才够。
   你的池是按什么尺寸算的？

5. **82fps 是否必须要求传感器直接输出 1280×720？**
   还是说"传感器 1920×1080 + VPSS 缩到 1280×720"也能到 80fps？
   （我实测 VI=1920×1080 时只有 33.7fps，若 VPSS 能绕开这个限制请指正。）

6. **`DIAG ISP pub fps=30.000000` 在你那版也是 30**，说明这个值不影响帧率，对吗？
   还是说 82fps 其实靠的是 `MAIX_SENSOR_FPS=80` 之外的某个设置？

---

## 7. 环境信息

| 项 | 值 |
|---|---|
| 板卡 | LicheeRV Nano / SG2002（1×RISC-V C906 @1GHz，128MB） |
| 系统 | Buildroot 2023.11.2，kernel `5.10.4-tag-` |
| 传感器 ini | `/mnt/data/sensor_cfg.ini` → `GCORE_GC4653_MIPI_4M_30FPS_10BIT`（代码里覆盖成 720P_60FPS） |
| 时钟 | `devmem 0x030028c4 32 0x00210001`（原始 `0x00200001`） |
| 工具链 | Xuantie-900 musl gcc 10.2.0，`riscv64-unknown-linux-musl-` |
| 温度 | 55–74°C；74.5°C 触发保护停机 |
| 原版二进制 | `/code/camera/modules/record80/camera_36record`（594,584 B，已 strip，无符号） |

---

## 8. 附：这个仓库里还改了什么（不影响上面的问题）

- `detect_enable` / `display_enable` / `record_enable` 三个 YAML 开关
- 检测框在 JPEG 编码前画进 Y 平面（已用固定测试框验证链路可用）
- 采样用 `CVI_SYS_MmapCache`（未缓存的 `CVI_SYS_Mmap` 单帧要 223ms）
- 开机自启 `/etc/init.d/S99flycamera` + `config_boot.yaml`（`run_seconds: 0` 常驻）

这些都已板端验证，不在本次求助范围内。
