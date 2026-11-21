# 板端运维脚本（拷贝到板子 `/code/fly_LicheeRV/tools/board/` 下用）

这五个脚本是**在板子上跑**的，用来验收、诊断、瘦身。它们不属于运行必需文件，
但出问题时比 ssh 手敲命令快得多，所以跟着仓库一起维护、一起部署。

## 部署（在 Windows / WSL 上）

```powershell
ssh -i $env:USERPROFILE\.ssh\licheepi_root_ed25519 root@10.133.124.1 "mkdir -p /code/fly_LicheeRV/tools/board"
scp -i $env:USERPROFILE\.ssh\licheepi_root_ed25519 `
    E:\study\LicheeRV\licheeRV-nano-greenTarget\tools\board\* `
    root@10.133.124.1:/code/fly_LicheeRV/tools/board/
```

脚本会自己按 `../..` 推断项目根目录（也可以用 `FLY_ROOT=/code/fly_LicheeRV` 覆盖），
所以放在 `<root>/tools/board/` 下直接 `sh` 就行。

## 五个脚本

| 脚本 | 什么时候用 | 用法 |
|---|---|---|
| `board_uart_check.sh` | **收不到电控数据时第一时间跑**。四层一次查完：① 各 UART 内核 rx/tx 计数 ② 停相机后抓 3 秒原始字节 + hexdump ③ UART1_RX 焊盘电平高速采样（判线上有没有跳变）④ 设备树 serial 节点 / pinctrl / gpiochip，最后直接给结论 | `sh tools/board/board_uart_check.sh`<br>`sh ... /dev/ttyS2`（指定设备）<br>`sh ... -k`（不停相机） |
| `board_verify_no_files.sh` | 验收「没触发就不产生文件」：只启动不触发跑 20 秒，检查 `record/` 是否为空 | `sh tools/board/board_verify_no_files.sh` |
| `board_verify_trigger.sh` | 验收「一次飞行一个文件夹」：用 PTY 假电控触发，检查会话目录里 `frames.csv` / `video.avi` / `video_frames.csv` / `health.csv` / `meta.txt` 是否齐全 | `sh tools/board/board_verify_trigger.sh`（需要 `/tmp/fake_mcu_pty.py`） |
| `board_check_session.sh` | 查某次会话为什么开/关：列出 `record/sess_*` 的文件大小与 `meta.txt` 里的 `close_reason`、行数、视频区间 | `sh tools/board/board_check_session.sh` |
| `board_slim.sh` | 板端瘦身：删文档、旧录制数据、手工备份，只留运行必需文件（会保留 `tools/` 本身） | `sh tools/board/board_slim.sh --dry`（先看会删什么）<br>`sh tools/board/board_slim.sh` |

## 约定

- **不往板端放源码/文档**：源码和文档在仓库里，板端只跑二进制。
- **不留 `*.bak`**：回退一律用 git（`git checkout -- <file>` / `git revert`）。
- 数据是产物：`record/` 里都是可重跑生成的东西，不需要备份。
- 这些脚本只读或短时操作，唯一会动文件的是 `board_slim.sh`（先 `--dry` 看一遍更稳）。
