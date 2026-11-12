# LicheeRV 连接、热点配置与摄像头预览指南

适用对象：本项目使用的 LicheeRV Nano + GC4653。
整理日期：2026-11-10。

本文只提供操作说明，没有修改板端网络、启动服务或保存任何真实热点密码。
USB 地址依据这块板子此前的实际连接结果；无线地址依据用户贴出的 ifconfig，
不是承诺以后都不变。手机菜单和镜像脚本可能因版本不同而不同。

## 1. 先分清：供电、局域网和互联网

- 供电：让板子和程序运行，不代表电脑能收到画面。
- 局域网连接：电脑和板子能互相访问，就可以 SSH 和预览。
- 互联网：访问外部网站所需，SSH 和本项目网页预览不要求互联网。

USB 数据线本身可以在电脑与板子之间建立网络，不需要 Wi-Fi。
手机热点即使没有移动数据，只要允许设备之间通信，也可以作为局域网。

## 2. 地址速查表

| 连接方式 | 板端接口/地址 | 电脑如何连接 | SSH | 浏览器预览 |
|---|---|---|---|---|
| USB NCM，首选 | usb1：10.133.124.1 | 数据线连接电脑与板子 | ssh root@10.133.124.1 | http://10.133.124.1:8081/ |
| USB RNDIS，备用 | usb0：10.133.125.1 | 同一条 USB 数据连接暴露的另一网卡 | ssh root@10.133.125.1 | http://10.133.125.1:8081/ |
| 当前外部 Wi-Fi | wlan0：此前为 172.20.168.30 | 电脑接入允许访问板子的网络 | ssh root@172.20.168.30 | http://172.20.168.30:8081/ |
| 手机热点 | wlan0：由手机 DHCP 分配，需重新查 | 板子和电脑都连接该热点 | ssh root@实际热点分配IP | http://实际热点分配IP:8081/ |
| 便携路由器 | wlan0：由路由器分配，可保留租约 | 板子和电脑接同一路由器 | ssh root@实际路由器分配IP | http://实际路由器分配IP:8081/ |
| 有线以太网 | eth0：用户输出里没有 IPv4 | 需有对应硬件、网线和网络 | 查到地址后连接 | 查到地址后访问 |
| 串口控制台 | 无 IP | USB-TTL 连接板子串口 | 不使用 SSH | 串口不传本项目网页画面 |

此前 Windows USB 网卡地址为 10.133.124.100 和 10.133.125.100。
连接 SSH 时输入板子末尾为 .1 的地址，不要连接电脑自己的 .100。

这些 USB 地址适用于当前板子/镜像配置；换板、重刷镜像或改 USB 网段后要重新核对。
172.20.168.30 属于先前的无线环境，换成手机热点后不能继续照抄。

## 3. 没有 Wi-Fi 的室外：USB 直连

1. 使用能传数据的 USB 线，连接板子的 USB 数据接口和电脑。
2. 等待系统启动及 Windows 识别 USB 网卡。
3. 在 Windows CMD 或 PowerShell 中输入：

~~~powershell
ping 10.133.124.1
ssh root@10.133.124.1
~~~

输入板端登录密码。输入密码时终端不显示字符是正常现象。
如 NCM 不可用，再试：

~~~powershell
ping 10.133.125.1
ssh root@10.133.125.1
~~~

4. 电脑浏览器访问：

http://10.133.124.1:8081/

备用地址：http://10.133.125.1:8081/

条件：摄像头程序已启动、网页预览已开启，服务监听该接口。
只有充电功能的线，或者板子只接充电宝而没有与电脑建立数据连接，都不能这样访问。

## 4. SSH 提示符：不要无意中嵌套登录

Windows 的提示符通常类似：

~~~text
C:\Users\admin>
PS C:\Users\admin>
~~~

输入 ssh 并登录成功后，板端通常显示：

~~~text
#
~~~

此时已经在板子上，不需要再输入 ssh root@另一个板端IP。
那样通常是在板子内部再次登录自己，不能证明电脑到无线地址的链路可用。

例如此前操作：

~~~text
Windows -> ssh 10.133.124.1 -> 板端 #
板端 # -> ssh 10.133.125.1 -> 同一板子
退出一层后仍是板端 # -> ssh 172.20.168.30 -> 同一板子
~~~

验证电脑无线连接时，应新开一个 Windows 终端，再运行无线 IP 的 ssh。
保持原 USB 会话作为恢复通道，测试成功后再考虑拔 USB。
退出当前一层 SSH：

~~~sh
exit
~~~

Ctrl+C 一般只中断当前命令，不等于退出 SSH。

## 5. 手机热点连接：推荐操作流程

### 5.1 准备手机

1. 开启个人热点，设置一个容易辨认的名称。
2. 初次测试优先开启 2.4 GHz/兼容性模式；是否支持其他频段以实际板卡和驱动为准。
3. 优先使用 WPA2-Personal 或兼容模式，避免初次调试仅允许 WPA3。
4. 设置不少于 8 个字符的密码；不要把真实密码提交到 Git。
5. 暂时关闭热点自动关闭功能，保持热点开启。
6. 让 Windows 电脑也连接该手机热点。

不需要为了预览摄像头开启移动数据，但部分手机存在热点策略限制。
同一热点也可能启用客户端隔离，遇到不能互访时参考排障部分。

### 5.2 先通过 USB 登录板子

在 Windows 运行：

~~~powershell
ssh root@10.133.124.1
~~~

后面的 Linux 命令在板端 # 提示符下执行。
不要通过即将修改的无线会话配置 Wi-Fi，USB 可以防止配置失败后失联。

### 5.3 核对当前镜像的持久配置入口

本工作区已有镜像使用说明记录：

- /boot/wifi.ssid：Wi-Fi 名称。
- /boot/wifi.pass：Wi-Fi 密码。
- /boot/wpa_supplicant.conf：如果存在，启动时优先覆盖系统配置。
- /etc/wpa_supplicant.conf：运行配置，可能被上述 boot 配置重建。
- /etc/init.d/S30wifi：无线启动脚本。

板端先只读核对启动脚本：

~~~sh
ls -l /etc/init.d/*wifi*
grep -n 'wifi.ssid\|wifi.pass\|wpa_supplicant.conf' /etc/init.d/S30wifi
ls -l /boot/wifi.ssid /boot/wifi.pass /boot/wpa_supplicant.conf
~~~

最后一条中部分文件不存在不一定是错误。不要打印密码文件内容。
如果启动脚本不存在，或者没有上述配置入口，停止套用下面的方法，
先按该镜像实际脚本确定配置来源。

### 5.4 方法 A：boot 名称/密码文件

适用于 S30wifi 已确认支持 wifi.ssid/wifi.pass，
并且不存在优先覆盖它们的 /boot/wpa_supplicant.conf。

首次修改前备份已有配置。下面命令只在备份不存在时创建，避免覆盖旧备份：

~~~sh
for f in /boot/wifi.ssid /boot/wifi.pass /boot/wpa_supplicant.conf /etc/wpa_supplicant.conf; do
    if [ -f "$f" ] && [ ! -e "$f.before-hotspot" ]; then
        cp -p "$f" "$f.before-hotspot" || break
    fi
done
~~~

通过编辑器输入，不要把密码直接写到命令行：

~~~sh
vi /boot/wifi.ssid
~~~

文件只放一行热点名称，不加引号、不写 ssid=。

~~~sh
vi /boot/wifi.pass
~~~

文件只放一行热点密码，不加引号、不写 psk=。
vi 操作：按 i 输入；完成后按 Esc，再输入 :wq 回车保存。
不要把编辑器中的密码截图或上传。

如果 /boot/wpa_supplicant.conf 已存在，不能只改这两个文件：
按下一节修改那份优先级更高的配置。

### 5.5 方法 B：boot 完整无线配置

适用于 S30wifi 已确认会复制 /boot/wpa_supplicant.conf。
先按上一节备份，然后编辑：

~~~sh
vi /boot/wpa_supplicant.conf
~~~

下面只是示例，替换名称和密码，并保留镜像所需的已有全局配置。
已有 ctrl_interface 时保留其原值，不盲目改控制目录。

~~~conf
network={
    ssid="替换为手机热点名称"
    psk="替换为手机热点密码"
    key_mgmt=WPA-PSK
}
~~~

这份示例适用于 WPA2-Personal。名称或密码包含双引号、反斜杠时需按
wpa_supplicant 格式转义，不应直接粘贴产生无效配置。
不要保留允许自动连接任意开放网络的 network 块。
如果要保留原 Wi-Fi，可以用多个明确命名的 network 块，
并给手机热点配置更高的 priority，避免自动连回其他网络。

备份同样包含密码。保留在设备本地，不加入项目仓库。
boot 分区可能是 FAT，不能依赖 chmod 真正保护其中的文件。

### 5.6 使配置生效

确保手机热点已开，板子没有正在录像或执行其他重要任务。
如果摄像头正在运行，先用项目 stop.sh 正常停止，再重启：

~~~sh
cd /code/fly_LicheeRV
sh stop.sh
sync
reboot
~~~

USB SSH 会断开，这是重启的正常现象。等待系统启动，再从 Windows 重连：

~~~powershell
ssh root@10.133.124.1
~~~

不要直接重复启动第二个 wpa_supplicant 或 DHCP 进程。

### 5.7 查热点分配的地址

在板端执行：

~~~sh
ifconfig wlan0
wpa_cli -i wlan0 status
ip route
~~~

如 wpa_cli 无法连接控制接口，先看 ifconfig；不据此直接认定 Wi-Fi 失败。
如没有 ip 命令，可用 route -n 查看路由。

应观察：

- wlan0 有 IPv4 地址。
- wpa_cli 可用时：ssid 是你的热点名称，wpa_state=COMPLETED。
- 不要把回环地址 127.0.0.1 或 USB 地址当成热点地址。

也可以在手机热点的已连接设备列表中查板子地址，
但并非所有手机都显示 IP。

### 5.8 从电脑独立测试无线链路

在新开的 Windows 终端执行，将地址替换为刚查到的值：

~~~powershell
ping <板端热点IP>
Test-NetConnection <板端热点IP> -Port 22
ssh root@<板端热点IP>
~~~

Test-NetConnection 是 PowerShell 命令；CMD 中可只运行 ping 和 ssh。
尖括号及其中提示文字不应原样输入。

电脑浏览器访问：

~~~text
http://<板端热点IP>:8081/
~~~

例如实际查到 192.168.50.20，才使用：

~~~text
ssh root@192.168.50.20
http://192.168.50.20:8081/
~~~

192.168.50.20 只是示例，本文没有给板子设置这个地址。
确认电脑端无线 SSH 和网页都正常后，可断开 USB 数据连接；
板子必须保持供电，拔掉唯一供电线会导致关机。

## 6. 固定 IP：推荐 DHCP 保留

### USB

本板当前的 10.133.124.1 / 10.133.125.1 不依赖无线 DHCP，
适合随时恢复和调试。不能把它们当成无线地址填写。

### 便携路由器

推荐现场使用自己的便携路由器：

1. 板子和电脑都接入它。
2. 在路由器后台找到 DHCP 客户端/地址保留。
3. 用无线网卡 MAC 绑定一个属于该路由器 LAN 网段的地址。
4. 重连后核对租约，之后使用保留地址。

本板此前 wlan0 的 MAC 为 14:0A:02:D9:79:55。
以实际 ifconfig wlan0 输出为准，不要使用 usb0/usb1 的 MAC 绑定无线租约。

### 手机热点

是否支持固定租约取决于手机，不能保证每天地址不变。
支持地址保留时按 MAC 绑定；不支持时保持 DHCP，每次查地址。
不要把原来的 172.20.168.30 写死到新热点网段。

板端静态 IP 必须同时确认热点子网、掩码、网关、DHCP 池和地址冲突，
且必须匹配镜像的启动/DHCP 脚本。本文不提供猜测网段的静态配置，
避免重启后 DHCP 覆盖或地址冲突。需要稳定无线地址时优先使用支持保留租约的路由器。

### 固定名称作为辅助

可以尝试：

~~~powershell
ssh root@licheervnano-857c.local
~~~

浏览器：http://licheervnano-857c.local:8081/

前提：设备名称一致、mDNS 服务运行、电脑支持解析、多播未被网络阻断。
名称可能解析到 USB 或 Wi-Fi；.local 不等于无线，也不保证跨子网可用。
要确认无线链路，使用实际 wlan0 IP 测试。

## 7. 摄像头画面与开机自启

上电、网络连通、摄像头启动、预览开启，这四个条件缺一不可。

本地源码中：

- 手动运行使用 /code/fly_LicheeRV/config.yaml。
- 开机自启使用 /code/fly_LicheeRV/config_boot.yaml。
- 建议 display_enable: true，web_ip: auto。
- web_ip 只控制服务绑定地址，不设置 SSID、密码或网卡 IP。
- web_ip: usb 当前仅绑定 10.133.124.1，不应期待备用 USB 地址也可访问。
- 0.0.0.0:8081 表示监听所有接口，浏览器应输入实际板端 IP，不输入 0.0.0.0。

SSH 登录板端后查询：

~~~sh
ps | grep '[c]amera'
netstat -lnt | grep 8081
tail -n 40 /tmp/fly_boot.log
tail -n 40 /tmp/fly_licheerv_capture.log
~~~

如果没运行且确认没有其他相机程序占用：

~~~sh
cd /code/fly_LicheeRV
sh start.sh --check-config
sh start.sh 120
~~~

这次手动运行约 120 秒，停止后网页也会停止。
已自动运行时不要重复 start，按需先用 stop.sh。

当前仓库的 boot/S99flycamera 最多等 wlan0 地址约 30 秒，
等待结束后即使没拿到无线地址也会尝试启动。
所以没有 Wi-Fi 不代表 USB 预览不能运行，但自启可能有所延迟。
该脚本是否已安装到板端 /etc/init.d，仍须实际检查，不能仅凭仓库文件认定已部署。

## 8. 常见故障

| 现象 | 排查方向 |
|---|---|
| USB 无网卡 | 换数据线、检查 USB 接口和 Windows 网卡驱动 |
| 能 SSH，浏览器不通 | 检查 camera 进程、display_enable、web_ip、8081 监听和日志 |
| 热点已连接但电脑访问不到 | 确认电脑也连该热点、IP 是否变化、手机是否隔离客户端 |
| 电脑和板子连同名校园 Wi-Fi 仍不通 | 同名 Wi-Fi 不保证设备互通，可能有 VLAN、隔离或防火墙 |
| wlan0 没 IPv4 | 确认热点开启、配置优先级、频段/安全模式、DHCP |
| 修改 /etc 配置重启后失效 | 检查 boot 配置是否在启动时覆盖它 |
| SSH 一直有心跳却数据不更新 | 网络连接与程序运行状态分开查，查看摄像头日志 |
| VPN 开启后不通 | 查看电脑路由；必要时临时关闭 VPN 再测试，不盲目修改路由 |
| .local 无法访问 | 改用实际 IP，mDNS 可能未启动或被阻断 |
| Ping 不通但 SSH 可用 | 网络可能禁用 ICMP，不把 Ping 作为唯一标准 |

Windows 可查询：

~~~powershell
ipconfig
route print
Test-NetConnection <板端实际IP> -Port 22
Test-NetConnection <板端实际IP> -Port 8081
~~~

首次连接新 IP 出现主机指纹提示时，先确认设备身份。
同一板子不同接口可能使用同一个 SSH 主机密钥，这与多网卡情况相符。
如果原来保存的主机密钥发生变化，不要直接忽略警告。

## 9. 配置失败的恢复

1. 保留/恢复 USB 数据连接，从 Windows SSH 到 10.133.124.1。
2. 找到本次改过的 boot 配置及 .before-hotspot 备份。
3. 仅恢复本次改过的对应文件，不批量覆盖其他网络配置。
4. 若文件是这次首次新建、原来不存在，应先确认后移走该新增配置，
   而不是恢复一个不存在的备份。
5. sync 后在没有重要任务运行时重启，重新查 wlan0。

恢复已有文件的例子（仅在对应备份确实存在且需要恢复时）：

~~~sh
cp -p /boot/wifi.ssid.before-hotspot /boot/wifi.ssid
cp -p /boot/wifi.pass.before-hotspot /boot/wifi.pass
sync
reboot
~~~

如果实际使用方法 B，应恢复那份 wpa_supplicant.conf 的备份。
备份也是敏感文件，不上传、不提交。

## 10. 推荐使用场景

- 室内开发/故障恢复：USB NCM，10.133.124.1。
- 无外部网络、电脑在旁边：USB，无需互联网。
- 临时室外无线预览：手机热点，电脑和板子同时接入，每次核对 IP。
- 需要长期固定无线地址：便携路由器 + DHCP 地址保留。
- 仅独立供电、不连电脑也没有无线：程序可独立运行；没有通道就不能实时预览。
- 离线留存：预先确认录像开启、空间足够，之后取回文件；录像与实时预览不同。

无线可达距离受热点/AP 覆盖、干扰和网络拓扑限制，不等于手机有流量就能任意远连接。
通过公网远程访问属于另一项网络部署任务，本指南不配置端口映射或公网 SSH。
