# my_websocket

一个基于 ESP-IDF v5.5.3 的 ESP32-C5 WebSocket 客户端示例，支持：

- `ws://gogo.uno:1885`
- `wss://gogo.uno:1886`

当配置为 `wss://` 时，程序会自动加载 [AAA-Certificate-Services.pem](main/AAA-Certificate-Services.pem) 证书进行 TLS 校验，并发送 `hello from esp32c5 with ssl`。

## 1. 环境准备

- 开发板：`ESP32-C5`
- ESP-IDF 版本：`v5.5.3`
- 建议系统：Linux

请先完成 ESP-IDF v5.5.3 的安装，并确认本机可使用对应工具链。

> 使用`wscat`客户端订阅了`/#`之后能收到服务器的所有消息，包含客户端的连接与断开
```sh
# 安装wscat
npm install -g wscat
```
```sh
# 连接ws服务器
wscat -c ws://gogo.uno:1885
```
连接成功后再输入`/#`订阅所有消息

## 2. 克隆项目，激活虚拟环境

```bash
# 克隆项目
git clone https://github.com/Orionxer/esp_websocket
```
```sh
# 进入项目
cd my_websocket
```

```bash
# 激活 ESP-IDF v5.5.3 环境：
source ~/.espressif/tools/activate_idf_v5.5.3.sh
```

## 3. 选择 ws 还是 wss

打开配置菜单：

```bash
idf.py menuconfig
```

进入：

```text
My WebSocket Configuration
```

重点配置以下几项：

- `WebSocket server URI`
- `WebSocket message`
- `Wi-Fi SSID`
- `Wi-Fi password`

如果选择 `ws`：

```text
WebSocket server URI = ws://gogo.uno:1885
WebSocket message    = hello from esp32c5
```

说明：

- 程序会使用明文 WebSocket 连接
- 程序会发送你配置的 `WebSocket message`

如果选择 `wss`：

```text
WebSocket server URI = wss://gogo.uno:1886
```

说明：

- 程序会自动启用 TLS
- 程序会自动加载 `main/AAA-Certificate-Services.pem`
- 程序会忽略 `WebSocket message` 的运行时发送内容，改为发送：

```text
hello from esp32c5 with ssl
```

## 4. 编译烧录验证

编译工程：

```bash
idf.py build
```

烧录并查看串口日志：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

如果你的串口不是 `/dev/ttyUSB0`，请替换成实际端口。

验证重点：

- Wi-Fi 成功连接并获取 IP
- 当 URI 为 `ws://gogo.uno:1885` 时，能够连接并发送配置的普通消息
- 当 URI 为 `wss://gogo.uno:1886` 时，能够连接并发送 `hello from esp32c5 with ssl`
- 串口日志中可以看到当前走的是 `WS` 还是 `WSS`
