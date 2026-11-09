# GC4653 标定模块

third_party/hik_calibration 是海康 MVS USB 工业相机工具，不能直接运行在 LicheeRV
的 GC4653 上。本项目增加了主机侧 gc4653_calibration.py，通过板端已有的
/snapshot.jpg 获取图片，在 PC 上完成棋盘格标定。

## 依赖

主机需要 Python 3 和 OpenCV：

~~~powershell
python -c "import cv2; print(cv2.__version__)"
~~~

## 在线采集标定图片

先在板端启动预览，确保 display_enable: true：

~~~sh
sh start.sh
~~~

然后在项目根目录运行：

~~~powershell
python tools/gc4653_calibration.py --calib
~~~

默认抓图地址是：

~~~text
http://10.133.124.1:8081/snapshot.jpg
~~~

按键：

- s：当前检测到完整棋盘格时保存；
- d：删除最近保存的图片；
- Esc：结束采集并开始计算。

默认棋盘参数是横向 11、纵向 8 个内角点，方格边长 45 mm。必须根据实际棋盘修改：

~~~powershell
python tools/gc4653_calibration.py --calib --board-width 11 --board-height 8 --square-size 0.045 --min-images 20
~~~

输出默认保存到：

~~~text
calibration/camera_intrinsics.yaml
~~~

## 离线重新计算

如果已经有图片：

~~~powershell
python tools/gc4653_calibration.py --calib --offline --save-dir calibration_images
~~~

## 写回板端配置

确认 RMS 和图片覆盖范围合理后，再显式写回两个配置文件：

~~~powershell
python tools/gc4653_calibration.py --calib --offline --save-dir calibration_images --apply-config
~~~

脚本会先创建 config.yaml.bak-calib 和 config_boot.yaml.bak-calib，
然后更新 cam_fx、cam_fy、cam_cx、cam_cy。畸变系数会保存到
calibration/camera_intrinsics.yaml，但当前实时 gridToAngleDeg() 尚未应用畸变校正。

## --calib 的运行边界

这个 --calib 是主机 Python 工具的参数，不是板端 RISC-V camera 二进制参数。
板端只负责提供当前 GC4653 的抓图，OpenCV 棋盘格计算在主机上完成。
