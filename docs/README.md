# 文档索引

> 先看 [../README.md](../README.md)。本目录只放「需要时再查」的细文档。
> 每个文件都标了状态：**现行** = 与当前代码一致；**历史** = 当时的取证/评审记录，仅作参考。

## 现行

| 文档 | 什么时候看 |
|---|---|
| [NETWORK_CONNECTION_GUIDE.md](NETWORK_CONNECTION_GUIDE.md) | 连不上板子：USB / Wi-Fi / 手机热点 / 固定 IP |
| [UART_PROTOCOL.md](UART_PROTOCOL.md) | 串口协议 v1（ASCII 文本帧，抓包排障用） |
| [UART_PROTOCOL_V2_DESIGN.md](UART_PROTOCOL_V2_DESIGN.md) | **串口协议 v2（出厂默认，28/33 字节二进制帧 + CRC16）** |
| [uart_v2_parser_example.c](uart_v2_parser_example.c) | 给电控的现成解析代码（单文件、零依赖） |
| [PROTOCOL_EXTENSION_PROPOSAL.md](PROTOCOL_EXTENSION_PROPOSAL.md) | **协议扩展提案**：TLV 扩展块 + 电控时间戳，向后兼容、以后加字段不用再改协议（需电控配合） |
| [DATA_RECORDING.md](DATA_RECORDING.md) | 飞行数据记录：预录触发、CSV 每一列的含义、怎么分析 |
| [DIAGNOSTICS.md](DIAGNOSTICS.md) | 运行状态 / 退出归因 / 9 个打印开关 / `status.sh` |
| [CALIBRATION.md](CALIBRATION.md) | 相机标定：怎么拿内参、怎么填进 config.yaml |
| [GC4653_CALIBRATION.md](GC4653_CALIBRATION.md) | GC4653 标定工具的按键与离线模式速查 |
| [READING_GUIDE.md](READING_GUIDE.md) | 逐段读代码的顺序（配合 `camera.cpp` 顶部的分节注释） |
| [architecture/](architecture/) | draw.io 架构图（总体/逐帧/算法/启动/诊断树） |
| [mindmap_status.png](mindmap_status.png) | 一页看懂现状（`tools/draw_mindmap.py` 生成） |
| [HANDOFF_WSL_BUILD_ENV.md](HANDOFF_WSL_BUILD_ENV.md) | WSL 里那棵 3.7 GB SDK 源码树是什么、怎么重建 middleware |

## 历史（当时的取证与评审，不建议作为现状依据）

| 文档 | 内容 |
|---|---|
| [history/HANDOFF_ISSUE_720p_80fps.md](history/HANDOFF_ISSUE_720p_80fps.md) | 2026-11 的 720P/80fps + 全绿画面取证过程，以及最终为什么改成"板端 .so + 源码编译 sensor/common" |
| [history/MIDTERM_REVIEW.md](history/MIDTERM_REVIEW.md) | 中期技术审核（Markdown 版） |
| [history/飞镖镖体视觉系统-中期技术审核报告.docx](history/) | 中期技术审核（Word 版，交付用） |

## 写文档的约定

1. **一个事实只写一处**：能放在 README 的就不重复写进子文档；子文档写"为什么"和"细节"。
2. **每个数字都要能复现**：写"实测 78.4fps"就要能指出是哪次、什么配置。
3. **过时了就挪进 `history/`**，不要留在原地改写成半对半错。
