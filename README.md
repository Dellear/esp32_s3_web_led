# 网页遥控灯（WebSocket 版）

用手机或电脑浏览器控制 ESP32-S3 板载 RGB 灯的颜色、亮度和开关，所有打开的页面实时同步。页面上还能看板子的资源占用、硬件信息，并测网速。

## 硬件与环境

- 板子：ESP32-S3 N16R8（16MB 闪存 + 8MB OPI PSRAM），板载 WS2812 灯在 GPIO48
- ESP-IDF v6.1
- 依赖组件（构建时自动下载）：`espressif/led_strip`、`espressif/mdns`

## 使用方法

1. 把 `main/secrets.h.example` 复制一份，改名为 `main/secrets.h`：

   ```powershell
   Copy-Item main\secrets.h.example main\secrets.h
   ```

   然后在 `main/secrets.h` 里填 WiFi 名称和密码（必须是 2.4GHz）：

   ```c
   #define WIFI_SSID     "你的WiFi名称"
   #define WIFI_PASSWORD "你的WiFi密码"
   ```

   `secrets.h` 已加入 `.gitignore`，不会被提交，不要上传到网上。`secrets.h.example` 会被提交，只放示例内容，不要在里面写真实密码。如果没改示例里的名称，板子启动时会提示还没填 WiFi，直接开热点。

2. 构建并烧录（第一次烧录或分区表变化后，先执行 `idf.py -p COM3 erase-flash`）：

   ```powershell
   . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
   idf.py build
   idf.py -p COM3 flash monitor
   ```

3. 连接 WiFi 时灯会慢闪黄色。连上后串口会打印地址，用浏览器打开 `http://esp32-led.local` 或串口里显示的 IP。

4. 如果 15 秒内连不上 WiFi，板子会开一个叫 `ESP32-LED` 的开放热点。手机连上后打开 `http://192.168.4.1`。

## 端口与接口

| 端口 | 用途 |
| ---- | ---- |
| 80   | HTTP 网页和接口 |
| 81   | WebSocket，实时收发灯控指令 |
| 5001 | 纯 TCP 测速，上行（电脑 → 板子） |
| 5002 | 纯 TCP 测速，下行（板子 → 电脑） |

HTTP 接口（80 端口）：

| 接口 | 说明 |
| ---- | ---- |
| `GET /` | 控制页面 |
| `GET /state` | 当前灯的状态 |
| `POST /set` | 改灯的状态，参数 `hex=RRGGBB&bright=0~255&on=0\|1` 都可选，放在请求体（表单格式）或网址里都行。只接受 POST，带 Origin 头且不是板子自己的网页发来的请求会被拒绝 |
| `GET /sys` | 运行时长、内存、PSRAM、闪存、WiFi 等信息 |
| `GET /speed/down?mb=1~32` | 网页测速下行，返回 mb 兆字节的数据 |
| `POST /speed/up` | 网页测速上行，请求体是任意数据，返回板子统计的字节数和用时 |

例子：

```sh
curl -X POST -d "hex=ff6600&bright=128&on=1" http://esp32-led.local/set
```

WebSocket 协议（`ws://板子IP:81/`）：

- 发送 `hex=ff6600&bright=128&on=1`：改状态，参数都可选，可任意组合
- 发送 `get`：只查询状态
- 接收 `{"hex":"#ff6600","bright":128,"on":true,"clients":2}`：任何人改动都会广播给所有客户端
- 接收 `{"error":"..."}`：指令有误，只发给发送者

浏览器只能从板子自己的网页连接 WebSocket（检查 Origin 头），防止别的网站借你的浏览器偷偷控制灯。服务器每 15 秒 ping 一次客户端，连续 2 次没回应就断开。

## 纯 TCP 测速

不经过 HTTP 服务器，用来和网页测速对比。电脑端运行：

```sh
python tcp_speed.py [板子IP] [秒数]
```

默认 IP 是 `esp32-led.local`，每个方向测 5 秒。也可以用 iperf2 测上行：`iperf -c 板子IP -p 5001`。

## 文件说明

```
├── CMakeLists.txt
├── partitions.csv          分区表：两个 3MB 程序分区 + FAT 分区
├── sdkconfig.defaults      默认配置：闪存、PSRAM、WebSocket、socket 数量等
├── tcp_speed.py            电脑端纯 TCP 测速脚本
├── main
│   ├── CMakeLists.txt
│   ├── idf_component.yml   依赖组件
│   ├── main.c              主程序
│   ├── page.html           控制页面，嵌入到固件里
│   ├── secrets.h.example   WiFi 配置示例，复制为 secrets.h 后填写
│   └── secrets.h           WiFi 名称和密码（自己创建，不提交）
└── README.md
```

## 实现说明

- WebSocket 在 81 端口的独立任务里运行，网页测速时灯控照常响应，只有 80 端口的 HTTP 请求要等测速结束。
- WebSocket 心跳：下一次 ping（15 秒后）前没收到回应算一次失败，所以掉线的客户端要 30~45 秒才清理掉。
- `/set` 的参数不做 URL 解码。
- 被拒绝的 WebSocket 连接会先收到 403 响应再断开。
