# fly_LicheeRV | GC4653 80 FPS 原始图传

这是**独立**于旧版 `/code/camera/modules` 的小项目，基于已验证的 GC4653 80 FPS MJPEG 采集方案：保留摄像头取流、硬件 JPEG 编码、网页实时图传，现已恢复**可选本地录像**；不包含 NPU/YOLO、RAW 抓图和网页叠框。

- 1280 × 720，原有合法 36 MHz 摄像头时钟方案，约 80 FPS 硬件取流。
- 手动固定曝光 **3000 μs**，模拟/数字/ISP 增益 1 倍。
- 网页直接显示原始画面（MJPEG），图传显示 FPS 不等于采集 FPS。
- 保留摄像头互斥锁、MIPI 错误/温度保护和退出时恢复时钟。不依赖 `/code/camera` 目录，仅使用板卡系统自带 SDK 运行库；项目锁放在 `/code/fly_LicheeRV/.camera.lock`，不再调用旧版本。

## 板端使用

```sh
ssh root@10.133.125.1
cd /code/fly_LicheeRV
sh start.sh
```

### 录像模式（直接执行你要的命令）

```sh
cd /code/fly_LicheeRV
./camera --recoding          # 开启录像 + 网页预览；运行时长按 config.yaml 的 run_seconds（默认 120 秒）
./camera --recording         # 同义命令，兼容正确英文拼写
sh start.sh --recoding 20    # 快速测试：只录 20 秒
sh stop.sh                   # 另一 SSH 终端安全提前停止（也可在当前终端按 Ctrl+C）
ls -lh /code/fly_LicheeRV/recordings/
ffprobe -hide_banner "$(ls -t /code/fly_LicheeRV/recordings/*.avi | head -n 1)"
```

录像目录：`/code/fly_LicheeRV/recordings/`。每次录制自动按时间和 PID 生成独立 `.avi` 文件；格式为 **1280×720 MJPEG/AVI（无音频）**，复用网页显示的同一条硬件 JPEG 编码输出，不重复打开摄像头。录像文件不是 H.264/MP4，视频播放速率按真实编码 PTS 计算。大容量录像会持续占用 SD 卡空间，建议定期人工清理。正常结束或 Ctrl+C 时，会先写完 AVI 索引，再由原有启动脚本释放相机锁、恢复合法 36 MHz 时钟。不可直接 `kill -9`，否则文件可能不能播放。

启动后终端打印 `WEB_READY http://<板端IP>:8081/`，电脑浏览器打开该网址。默认运行 120 秒（可传 20~300），终端 `Ctrl+C` 停止。`/snapshot.jpg` 是静态 JPEG，`/live.mjpg` 是 MJPEG 动态视频。采集帧率等数据输出在终端，完整日志临时存放 `/tmp/fly_licheerv_capture.log`。


## 直接调参（板端 config.yaml）

```sh
cd /code/fly_LicheeRV
vi config.yaml              # 修改参数（如 manual_exposure_us: 3000）
sh start.sh --check-config  # 先检查：不占用摄像头
sh start.sh                 # 以 YAML 的 run_seconds 运行
```

修改参数后需要停止当前取流再启动；**不是热更新**。已有板端 `config.yaml` 在下一次 `deploy.ps1` 更新程序时会**保留而不被覆盖**。

| 参数 | 默认值 | 作用 |
|---|---|---|
| `exposure_mode` | `manual` | 固定手动曝光；设 `auto` 开启 ISP 自动曝光 |
| `manual_exposure_us` | `3000` | 手动曝光时间，100～10000 μs |
| `manual_analog_gain_x1024` | `1024` | 模拟增益（1024 = 1 倍，最高 16384） |
| `manual_digital_gain_x1024` | `1024` | 数字增益（最高 4096） |
| `manual_isp_gain_x1024` | `1024` | ISP 增益（最高 4096） |
| `auto_exposure_max_us` | `0` | 自动曝光的时间上限，0 为 ISP 默认上限 |
| `auto_*_gain_max_x1024` | `0` | 自动增益上限，0 使用 ISP 默认（具体项见 YAML） |
| `black_level_mode` | `manual` | 黑电平手动 / `default` 保留 ISP 设置 |
| `black_level_r/gr/gb/b` | `256` | 四个通道黑电平，范围 0～4095 |
| `jpeg_quality` | `85` | 图传 JPEG 画质，75～90 |
| `run_seconds` | `120` | 默认运行时长，20～300 秒，可通过 `sh start.sh` 临时覆盖 |

本项目依旧固定 **36 MHz / 80 FPS** 合法取流方案。YAML 不允许随意改时钟、分辨率，避免出现已知的花屏问题。仅支持文件中展示的平铺键值对，不支持嵌套 YAML 或 `eval`。错误配置会在接触摄像头之前报错。

## 工程文件

工程只新增一个 `src/` 源码目录：`src/camera.cpp`（取流/网页/录像入口）和 `src/avi_mjpeg.h`（AVI 封装）。`camera`（RISC-V ELF）仍留在根目录，`start.sh`、`stop.sh`、`config.yaml`、`README.md` 也不移动；已有的录像 `recordings/` 保持原样。

电脑端位于 `D:\study\LicheeRV\fly_LicheeRV`，额外有 `build.sh`、`deploy.ps1`、`.gitignore` 和构建时产生的 `build/`。`build/`、编译出的 `camera`、模型和视频不提交 Git。

重新编译（Windows WSL）：`bash fly_LicheeRV/build.sh`；部署（PowerShell）：`powershell -ExecutionPolicy Bypass -File D:\study\LicheeRV\fly_LicheeRV\deploy.ps1 -Board root@10.133.125.1`。

**未来模型接口**：在 `camera.cpp` 的 VI 帧处理处接 C++ 图像预处理、NPU、后处理和在原帧上画框；当前故意不实施算法推理。

## 运行与解锁（无需手动 rm 锁）

```sh
cd /code/fly_LicheeRV
sh start.sh        # 默认按 config.yaml 中 run_seconds: 120 运行
sh stop.sh         # 另一个 SSH 终端中随时停止当前摄像头，自动恢复时钟和释放锁
```

`Ctrl+C` 会自动停止和释放；SSH 中断 / 异常退出后，再运行 `sh start.sh` 会检查失效锁：若已无存活的采集进程则自动释放；如果上次采集仍在运行，会提示使用 `sh stop.sh`，**不会强行抢占正在使用的相机**。`stop.sh` 只停止本项目自己的进程，不结束其他摄像头程序。即使监督脚本被强制杀死，保底守护逻辑也会在超时后尝试安全回收。


## 电脑浏览器 ERR_EMPTY_RESPONSE（VPN / 两个标签页）

已修复原来 MJPEG 长连接阻塞其他浏览器请求的问题：现在最多 4 个独立连接，单个浏览器视频流约 20 FPS，**摄像头 VI 取流和 JPEG 编码仍保持约 80 FPS**。当摄像头已经停止时，8081 不提供网页；先启动 `sh start.sh`。

如果 **iKuuuVPN TUN** 开启，Windows 访问 `192.168.1.102` 可能走 `198.18.0.1` 虚拟网卡，网页出现 ERR_EMPTY_RESPONSE。解决办法一：在 VPN 中允许局域网直连（排除 `192.168.1.0/24`）。

无需更改 VPN 配置也可以通过 USB SSH 转发：在 Windows CMD 中输入以下**一条命令**（保持终端打开）：

```bat
ssh -L 18081:192.168.1.102:8081 root@10.133.125.1 "cd /code/fly_LicheeRV && sh start.sh"
```

然后浏览器访问 `http://127.0.0.1:18081/`；终端按 Ctrl+C 可以停止。`sh start.sh` 默认按 YAML 运行 120 秒。如果 USB SSH 不通，要先排查 USB 网络连接。
