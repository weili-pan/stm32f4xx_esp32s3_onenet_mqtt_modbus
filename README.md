# STM32F407 + ESP32-S3 Modbus 环境监测与 OneNET MQTT OTA

## 项目名称与简介

**项目名称：** `stm32_modbus_esp32s3_mqtt_ota`

本项目是一个双 MCU 的环境监测与远程升级示例：

- **STM32F407ZGT6** 负责实时任务：作为 Modbus RTU 主站轮询两路 RS485 温湿度传感器，在 240×320 ST7789 屏上用 LVGL 显示数据与链路状态，并支持按键开关背光。
- **ESP32-S3** 负责网络任务：连接 2.4 GHz WiFi 和 OneNET MQTT，上报设备状态，接收云端下发的 OTA 任务，通过 HTTPS 下载固件，再经 UART 分片转发给 STM32。
- **STM32 Bootloader（stm_ota）** 负责固件落地：把收到的固件暂存到 W25Q64，校验整包 CRC32 后搬运到内部 Flash 应用区，校验通过再跳转执行。

这样做的目的是把"实时采集与显示"和"网络协议、TLS、MQTT、HTTPS"解耦：STM32 不承担网络栈，ESP32 不承担实时控制，双方通过一条带 CRC 的二进制串口协议通信。

项目同时实现了 ESP32-S3 自身固件的 OTA 分区升级路径，但当前分区大小需要调整后才能实际使用（见[注意事项与限制](#注意事项与限制)）。

---

## 功能特点

- **双 MCU 架构**：STM32F407 处理 Modbus、LCD、按键等实时任务；ESP32-S3 处理 WiFi、MQTT、HTTPS、OTA。
- **Modbus RTU 主站**：非阻塞状态机轮询两个从站（0x01 / 0x02），支持超时、重试、离线判定。
- **本地可视化**：ST7789 240×320 竖屏 + LVGL 8.3，显示两路温湿度、WiFi/MQTT 状态、RSSI、设备在线状态和 OTA 就绪状态。
- **中文字体**：使用 `lv_font_conv` 生成的点阵字体，不依赖芯片内置中文字库。
- **OneNET MQTT**：OneJSON 属性上报/设置，HMAC-SHA1 动态 Token，SNTP 自动对时。
- **HTTPS OTA 下载**：支持 URL、版本号、大小、MD5 校验；固件源可以是任意静态文件服务器/对象存储。
- **可靠的 UART 固件传输**：`AA 55` 帧头 + CRC16/MODBUS + 序号 + ACK + 超时重传。
- **掉电安全的 STM32 升级**：W25Q64 暂存 + 整包 CRC32 + 状态机（EMPTY/RECEIVING/READY/CONSUMED），只有校验通过才会擦写 App 区。
- **ESP32-S3 自升级**：写入 OTA 分区并切换启动分区，重启后生效。
- **双向状态感知**：ESP32 每秒向 STM32 推送链路状态，STM32 3 秒收不到就回落为离线显示。
- **PC 端 UI 模拟器**：`lv_st7789_example` 可在 Windows 上无硬件预览界面版式。

---

## 系统架构与工作流程

### 系统框图

```text
                RS485 / Modbus RTU (9600 8N1)
  +-----------+        +-----------------------+
  | 传感器 01 |--------|                       |
  +-----------+        |      STM32F407        |---- SPI2 ----> ST7789 240x320
                       |                       |
  +-----------+        |  Modbus 主站          |---- SPI1 ----> W25Q64 8MB
  | 传感器 02 |--------|  LVGL 界面            |                (仅 Bootloader 使用)
  +-----------+        |  UART 帧解析          |
                       +-----------+-----------+
                                   |
                          USART1 115200 8N1
                       AA 55 | TYPE | SEQ | LEN | DATA | CRC16
                                   |
                       +-----------+-----------+
                       |      ESP32-S3         |
                       |  WiFi STA / MQTT     |---- 2.4 GHz WiFi ----> OneNET
                       |  HTTPS 下载 / OTA    |---- HTTPS -----------> 固件服务器 / OSS
                       +-----------------------+
```

### 正常运行流程

1. STM32 上电后初始化 LCD、LVGL、Modbus 主站、按键、LED 和 USART1 帧解析。
2. STM32 轮流请求从站 0x01 / 0x02 的 2 个输入寄存器，把温湿度刷新到界面卡片。
3. ESP32-S3 连接 WiFi，SNTP 对时后生成 OneNET Token 并连接 MQTT。
4. ESP32 每秒向 STM32 发送 `LINK_STATUS` 帧，STM32 据此刷新 WiFi、RSSI、设备在线、OTA 就绪状态。
5. ESP32 每 30 秒向 OneNET 上报 `wifi_rssi`、`firmware_version`、`stm_link`。

### OTA 升级流程

```text
OneNET 控制台
   │  属性设置 thing/property/set
   │  {"params":{"url":...,"version":...,"size":...,"md5":...,"target":"stm32"}}
   ▼
ESP32-S3
   │  1. 解析 OTA 任务
   │  2. HTTPS 打开 url，检查状态码 200 / Content-Length
   │  3. 边下载边经 UART 发送
   ▼
STM32 Bootloader
   │  OTA_START -> ACK
   │  OTA_DATA(seq=0..N) -> ACK(seq)
   │  OTA_FINISH(crc32) -> CRC_RESULT
   │  4. 固件写入 W25Q64（4KB 暂存块）
   │  5. 回读 W25Q64 计算 CRC32
   │  6. 擦除内部 App 区并搬运
   │  7. 校验内部 Flash 后跳转到 0x08010000
   ▼
新固件运行，继续通过 ESP32 上报状态
```

OTA 进度会通过属性上报回 OneNET：ESP 侧使用 `ota_stage` / `ota_progress` / `ota_message`，Bootloader 侧使用 `ota_stage` / `ota_percent` / `msg`。

---

## 硬件与软件环境

### 硬件清单

| 器件 | 型号 / 参数 | 说明 |
|---|---|---|
| 主控 | STM32F407ZGT6 | 1 MB Flash / 192 KB RAM（128 KB SRAM + 64 KB CCM），外部 8 MHz 晶振 |
| 联网模块 | ESP32-S3 | 2.4 GHz WiFi，当前工程按 16 MB Flash 配置 |
| 外部 Flash | W25Q64 | 8 MB SPI NOR，接 STM32 SPI1，用于 OTA 暂存 |
| 显示屏 | ST7789 | 240×320 竖屏，SPI 接口 |
| RS485 收发器 | MAX3485 / 自动换向模块 | 接 STM32 USART2 |
| 调试器 | ST-Link V2/V3 | 烧录 STM32 |
| USB 线 | USB-Serial / USB-OTG | 烧录 ESP32-S3 并查看日志 |

推荐接线：

| 信号 | STM32F407 | ESP32-S3 / 外设 |
|---|---|---|
| 串口 TX | PA9 (USART1_TX) | GPIO18 (UART1_RX) |
| 串口 RX | PA10 (USART1_RX) | GPIO17 (UART1_TX) |
| 地 | GND | GND |
| RS485 TX | PA2 (USART2_TX) | 收发器 DI / 模块 RXD |
| RS485 RX | PA3 (USART2_RX) | 收发器 RO / 模块 TXD |
| RS485 DE/RE | PD4（可选） | 收发器 DE/RE |
| LCD SPI | PB10 (SCK) / PC3 (MOSI) | ST7789 SCL / SDA |
| LCD 控制 | PB0 CS / PB12 DC / PB11 RST / PB13 BL | **当前为占位宏，需按实际接线修改** |
| W25Q64 | PA5 SCK / PA6 MISO / PA7 MOSI / PB1 CS | 仅 Bootloader 使用 |
| 按键 | PA15 | 低电平有效，切换背光 |
| LED1 | PC13 | 低电平点亮，ESP 链路指示灯 |

> 注意：串口必须交叉连接（TX→RX、RX→TX），并且两块板必须共地。

### 软件环境

| 软件 / 库 | 版本 | 用途 |
|---|---|---|
| ESP-IDF | 5.5.3 | ESP32-S3 工程构建 |
| ESP-IDF target | esp32s3 | `idf.py set-target esp32s3` |
| FreeRTOS | ESP-IDF 内置 | ESP32 任务与队列 |
| esp-mqtt / esp_http_client / mbedTLS / cJSON | ESP-IDF 内置 | MQTT、HTTPS、哈希、JSON |
| STM32CubeCLT | 1.19.0 | STM32 CMake / Ninja / arm-none-eabi 工具链 |
| STM32 HAL | 1.8.5 | STM32 外设驱动 |
| LVGL | 8.3.11 | 本地 UI |
| CMake | 3.28.1（CubeCLT 内置） | STM32 构建 |
| Ninja | CubeCLT 内置 | STM32 构建 |
| Python | 3.11（ESP-IDF venv） | ESP-IDF 构建脚本 |
| STM32CubeMX / CLion | 可选 | 修改 `.ioc`、下载调试 |

ESP-IDF v5.5 已不再支持 MSYS/MinGW 环境下直接运行 `idf.py`，请使用 Windows CMD 的 ESP-IDF 环境。

---

## 项目目录结构

```text
stm32_modbus_esp32s3_mqtt_ota/
├─ README.md                        本文档
├─ STM32F407阿里云MQTT与WiFi OTA项目方案.docx   原始方案文档（正文目标实际为 OneNET）
│
├─ esp_mqtt_ota_connect/            ESP32-S3 工程（ESP-IDF）
│  ├─ CMakeLists.txt
│  ├─ partitions.csv                自定义分区表（factory + ota_0 + ota_1）
│  ├─ sdkconfig                     menuconfig 生成结果（含 WiFi/OneNET 凭据，勿提交）
│  ├─ main/
│  │  └─ main.c                     应用编排：WiFi、MQTT、下行 OTA、状态上报
│  └─ components/
│     ├─ get_wifi_ota/              WiFi STA + HTTPS 下载 + ESP 自升级
│     ├─ connect_stm/               ESP32 ↔ STM32 串口帧协议
│     └─ onenet_mqtt/               OneNET Token、MQTT 连接、OneJSON 属性收发
│
├─ stm_part/
│  ├─ stm_modbus/                   STM32 应用工程
│  │  ├─ Core/                      CubeMX 生成的启动、时钟、外设初始化
│  │  ├─ Components/
│  │  │  ├─ Modbus/                 Modbus RTU 主站状态机
│  │  │  ├─ RS485/                  USART2 + 可选 DE 控制
│  │  │  ├─ LCD/                    ST7789 SPI 驱动
│  │  │  ├─ UI/                     LVGL 界面与中文字体
│  │  │  ├─ Link/                   ESP32 状态帧接收与解析
│  │  │  ├─ KEY/                    背光按键
│  │  │  └─ LED/                    链路指示灯
│  │  ├─ Middlewares/lvgl/          LVGL 8.3.11 及移植层
│  │  ├─ cmake/                     工具链文件
│  │  ├─ CMakePresets.json          Debug / Release preset
│  │  └─ STM32F407xx_FLASH.ld       应用链接脚本（OTA 前必须改到 0x08010000）
│  │
│  └─ stm_ota/                      STM32 Bootloader 工程
│     ├─ Core/Inc/ota_config.h      地址、引脚、超时、缓冲区等全部配置
│     ├─ Core/Src/ota_boot.c        OTA 主流程、掉电安全状态机、Flash 搬运
│     ├─ Core/Src/ota_proto.c       串口帧协议、CRC16/CRC32、DMA 接收
│     ├─ Core/Src/w25q64.c          W25Q64 最小驱动
│     ├─ cmake/                     工具链文件
│     └─ CMakePresets.json          Debug / Release preset
│
└─ lv_st7789_example/               PC 端 LVGL 模拟器与 UI 参考稿
   ├─ download.png                  原始界面参考图（320×240 横屏）
   └─ ui/                           与 stm_modbus 对应的界面与字体源码
```

`build/`、`cmake-build-*/`、`.idea/` 等目录为构建产物或 IDE 缓存，不应提交到版本库。

---

## 安装与配置

### 1. ESP32-S3 工程

进入 `esp_mqtt_ota_connect/`，在 ESP-IDF 5.5 的 CMD 环境中执行：

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

需要配置的菜单：

**`WiFi and OTA download (get_wifi_ota)`**

| 配置项 | 说明 |
|---|---|
| `WiFi SSID` | 2.4 GHz WiFi 名称 |
| `WiFi password` | WiFi 密码 |
| `HTTP download timeout (ms)` | 单次 socket 读写超时，默认 15000 |
| `Allow ESP32S3 self-update` | 是否允许 `target=esp32s3` 的自升级 |

**`STM32 serial link (connect_stm)`**

| 配置项 | 默认值 | 说明 |
|---|---|---|
| UART port | 1 | UART0 保留给日志 |
| TX GPIO | 17 | ESP32-S3 → STM32 |
| RX GPIO | 18 | ESP32-S3 ← STM32 |
| Baud rate | 115200 | 必须与 STM32 USART1 一致 |
| OTA chunk size | 1024 | 256~4096 |
| ACK timeout | 500 ms | 超时重传当前分片 |
| Max retry | 3 | 单帧最大重传次数 |
| Link timeout | 10000 ms | 超过该时间没收到 STM32 帧即判链路断开 |

**`OneNET MQTT (onenet_mqtt)`**

| 配置项 | 说明 |
|---|---|
| ProductKey | OneNET 产品 ID |
| DeviceName | 设备名称 |
| DeviceSecret | 设备密钥，敏感信息 |
| MQTT broker URI | 默认 `mqtt://mqtts.heclouds.com:1883`；TLS 用 `mqtts://mqttstls.heclouds.com:8883` |
| Enable TLS | URI 为 `mqtts://` 时勾选 |
| Token lifetime | 默认 2592000 秒（30 天） |
| Topic 后缀 | 一般保持 `thing/property/post`、`thing/property/set` 等默认值 |

> 不要往 Kconfig 里填写带 `$sys/{...}` 的完整 topic。平台前缀由代码拼装；把 `$sys/` 写进 Kconfig 会导致 CMake 解析 `sdkconfig.cmake` 失败。只填写 `$sys/{ProductKey}/{DeviceName}/` 之后的后缀。

### 2. STM32 工程

在 `stm_part/stm_modbus` 和 `stm_part/stm_ota` 中使用 CMake Preset 构建。工具链不在默认 PATH 时，先执行：

```bash
export PATH="/d/stm32cubeclt/STM32CubeCLT_1.19.0/CMake/bin:/d/stm32cubeclt/STM32CubeCLT_1.19.0/Ninja/bin:/d/stm32cubeclt/STM32CubeCLT_1.19.0/GNU-tools-for-STM32/bin:$PATH"
```

然后：

```bash
cd stm_part/stm_modbus
cmake --preset Debug
cmake --build --preset Debug
# 产物：build/Debug/stm_modbus.elf

cd ../stm_ota
cmake --preset Debug
cmake --build --preset Debug
# 产物：build/Debug/stm_ota.elf
```

需要生成可直接下载的裸 bin 时：

```bash
arm-none-eabi-objcopy -O binary build/Debug/stm_modbus.elf stm_modbus.bin
arm-none-eabi-objcopy -O binary build/Debug/stm_ota.elf stm_ota.bin
```

主要可调参数：

- `stm_part/stm_modbus/Components/Modbus/modbus_master.h`
  - 从站地址、功能码、起始寄存器、寄存器数量、波特率、超时、重试次数。
- `stm_part/stm_modbus/Components/RS485/rs485.h`
  - `RS485_DE_ENABLE`：1 表示使用独立 DE/RE 收发器，0 表示自动换向模块。
- `stm_part/stm_modbus/Components/LCD/lcd_st7789.h`
  - CS / DC / RST / BL 引脚、颜色字节序、BGR 顺序、上电自检开关。
- `stm_part/stm_ota/Core/Inc/ota_config.h`
  - W25Q64 引脚、JEDEC ID、Flash 分区地址、OTA 超时、暂存块大小、协议常量。

### 3. 重要：OTA 前必须修改 STM32 应用链接地址

Bootloader 位于 `0x08000000`，应用区从 `0x08010000` 开始，Bootloader 跳转前会把 `SCB->VTOR` 设置为 `OTA_APP_ADDR`。因此 STM32 应用必须链接到 `0x08010000`。

当前 `stm_part/stm_modbus/STM32F407xx_FLASH.ld` 中仍是：

```ld
FLASH (rx) : ORIGIN = 0x8000000, LENGTH = 1024K
```

在启用 OTA 前应改为：

```ld
FLASH (rx) : ORIGIN = 0x08010000, LENGTH = 960K
```

否则把当前编译出的 bin 下发给 Bootloader 后，虽然能写入，但应用中的绝对地址仍然是 `0x08000000` 段，运行会异常。

### 4. OneNET 物模型配置

在产品物模型中定义以下属性（属性名必须与代码一致）：

**下行（用于 OTA 任务）**

| 属性 | 类型 | 说明 |
|---|---|---|
| `url` | string | 固件 HTTPS 直链，最长 255 字节 |
| `version` | string | 目标版本号，最长 32 字节 |
| `size` | int32 | 固件字节数，可省略（使用 Content-Length） |
| `md5` | string | 32 位十六进制 MD5，可省略 |
| `target` | string | `stm32`（默认）或 `esp32s3` |

**上行（ESP32 周期上报 / OTA 进度）**

| 属性 | 类型 | 说明 |
|---|---|---|
| `wifi_rssi` | int | WiFi 信号强度，单位 dBm |
| `firmware_version` | string | ESP32 当前固件版本 |
| `stm_link` | bool | ESP32 是否收到 STM32 的帧 |
| `ota_stage` | string | OTA 阶段：`downloading` / `success` / `failed`，Bootloader 还会发 `ready` / `boot` / `recover` / `receiving` / `installing` / `installed` / `error` / `abort` |
| `ota_progress` | int | ESP32 侧下载进度 0~100 |
| `ota_message` | string | ESP32 侧 OTA 附加信息 |
| `ota_percent` | int | Bootloader 侧进度 0~100 |
| `msg` | string | Bootloader 侧附加信息 |

> 未在物模型中定义的属性可能导致平台拒绝整条属性上报或设置报文。

### 5. 固件托管

代码只要求一个可访问的 HTTP(S) 直链，不需要自建网站：

- 测试：把 bin 放到一个目录，执行 `python -m http.server 8000`，URL 填 `http://<电脑局域网IP>:8000/stm_modbus.bin`。
- 正式：阿里云 OSS、腾讯云 COS 等对象存储，对象权限设为公共读，复制对象 URL。
- 必须返回 `200`，并带 `Content-Length`。
- 使用 HTTPS 时必须由公共 CA 签发证书；代码使用 ESP-IDF 内置证书 bundle 校验，不接受自签名证书。

---

## 使用方法

### 1. 首次烧录

1. 用 ST-Link 将 `stm_ota.elf` 烧到 `0x08000000`。
2. 修改 `stm_modbus` 链接脚本到 `0x08010000` 后编译，用 ST-Link 将 `stm_modbus.elf` 烧到应用区。
3. 用 USB 将 ESP32-S3 进入下载模式，执行：

```bash
cd esp_mqtt_ota_connect
idf.py -p COMx flash monitor
```

4. 连接 RS485 传感器、LCD、W25Q64，并确认所有板卡共地。

### 2. 启动与操作

- 上电后 STM32 显示界面，两张卡片分别显示传感器 01 / 02 的温度和湿度。
- 顶部显示 WiFi 连接状态；底部显示设备在线状态、RSSI、OTA 就绪状态。
- 按 PA15 按键切换 LCD 背光。
- PC13 LED 亮表示收到 ESP32 的链路状态帧；超过 3 秒未收到则熄灭，界面回落为离线。
- ESP32 串口日志会打印 WiFi IP、MQTT 连接结果、周期上报和 OTA 过程。

### 3. 云端下发 STM32 OTA

在 OneNET 控制台向 `thing/property/set` 下发：

```json
{
  "id": "1",
  "params": {
    "url":     {"value": "https://your-server/fw/stm_modbus.bin"},
    "version": {"value": "1.0.1"},
    "size":    {"value": 365172},
    "md5":     {"value": "0123456789abcdef0123456789abcdef"},
    "target":  {"value": "stm32"}
  }
}
```

ESP32 会：

1. 解析任务并立刻以 `downloading` 上报进度。
2. 通过 HTTPS 打开 URL，检查 `200` 和 `Content-Length`。
3. 发送 `OTA_START`，等待 STM32 Bootloader 的 ACK。
4. 按 1024 字节分片发送，每片等待 ACK，失败自动重传。
5. 下载完成后校验 MD5、长度，发送 `OTA_FINISH` 和 CRC32。
6. Bootloader 校验 W25Q64 中的整包 CRC32，搬运到内部 Flash，再次校验后跳转。
7. ESP32 上报 `success` 或 `failed`。

### 4. STM32 Bootloader 触发说明

Bootloader 进入下载模式的条件是：

- App 把 `RTC->BKP0R` 写成 `0x4F544131`（"OTA1"）后复位；或
- App 区无效；或
- 编译时把 `OTA_BOOT_WAIT_MS` 设为大于 0（台架调试模式，每次上电都先等 OTA）。

**当前 STM32 应用尚未实现写入 BKP0R 并复位的逻辑。** 因此完整远程 OTA 链路还差最后这一步。台架验证时可以临时把 `stm_ota/Core/Inc/ota_config.h` 中的：

```c
#define OTA_BOOT_WAIT_MS         0U
```

改为例如 `30000U`，重新烧录 Bootloader，然后在上电后的 30 秒窗口内从 OneNET 下发 OTA 任务。出货固件必须改回 `0U`。

### 5. ESP32-S3 自升级

下发 `target=esp32s3` 的任务，并在 menuconfig 中打开 `Allow ESP32S3 self-update`。ESP32 会：

1. 调用 `esp_ota_get_next_update_partition()` 获取目标 OTA 分区。
2. 边下载边写入 OTA 分区。
3. `esp_ota_end()` 校验镜像。
4. `esp_ota_set_boot_partition()` 切换启动分区并重启。

> 当前 `partitions.csv` 中 `ota_0` / `ota_1` 各 1 MB，而 Debug 构建的 `esp_mqtt_ota_connect.bin` 约 1.06 MB，会因镜像过大失败。需要缩小固件或重排分区表。

---

## 通信协议与接口说明

### 1. Modbus RTU

STM32 作为主站，USART2 / RS485，参数：

| 项目 | 值 |
|---|---|
| 波特率 | 9600 |
| 数据位 / 校验 / 停止位 | 8 / None / 1 |
| 从站地址 | 0x01、0x02 |
| 功能码 | 0x04（读输入寄存器） |
| 起始寄存器 | 0x0000 |
| 寄存器数量 | 2 |
| 回包超时 | 200 ms |
| 失败重试 | 3 次后判离线 |
| 请求间隔 | 20 ms |
| 分帧 | 3.5 字符静默时间，约 4 ms |

寄存器约定：

| 寄存器 | 含义 | 缩放 | 类型 |
|---|---|---|---|
| reg0 (0x0000) | 温度 | ×10，如 265 = 26.5 ℃ | int16，大端 |
| reg1 (0x0001) | 湿度 | ×10，如 580 = 58.0 %RH | uint16，大端 |

请求帧（8 字节）：

```text
[addr] [0x04] [0x00] [0x00] [0x00] [0x02] [CRC_L] [CRC_H]
```

正常回包（9 字节）：

```text
[addr] [0x04] [0x04] [T_H] [T_L] [H_H] [H_L] [CRC_L] [CRC_H]
```

### 2. MQTT 主题与 OneJSON

Topic 前缀由平台固定为 `$sys/{ProductKey}/{DeviceName}/`，代码中用 `TOPIC_PREFIX_FMT` 拼装。

| 方向 | 主题 | 用途 |
|---|---|---|
| 发布 | `$sys/{PK}/{DN}/thing/property/post` | 属性上报 |
| 订阅 | `$sys/{PK}/{DN}/thing/property/post/reply` | 上报响应 |
| 订阅 | `$sys/{PK}/{DN}/thing/property/set` | 属性设置（OTA 下发） |
| 发布 | `$sys/{PK}/{DN}/thing/property/set/reply` | 属性设置应答 |

属性上报示例：

```json
{
  "id": "12345",
  "version": "1.0",
  "params": {
    "wifi_rssi":        {"value": -55},
    "firmware_version": {"value": "1.0.0"},
    "stm_link":         {"value": true}
  }
}
```

代码会自动给每个扁平属性包一层 `{"value": ...}`；扁平写法会被平台以格式错误拒绝。

连接参数：

| 参数 | 值 |
|---|---|
| ClientID | DeviceName |
| Username | ProductKey |
| Password | Token |
| Token version | `2018-10-31` |
| Token 算法 | `base64(hmac_sha1(base64_decode(DeviceSecret), "et\nmethod\nres\nversion"))` |
| Resource | `products/{ProductKey}/devices/{DeviceName}` |
| et | 当前 Unix 时间 + Token 有效期 |

Token 依赖系统时间，ESP32 启动时会先通过 SNTP 对时。

### 3. STM32 ↔ ESP32 串口帧

固定帧格式（小端）：

```text
+--------+--------+--------+--------+--------+----------+--------+
| SOF(2) | TYPE(1)| SEQ(2) | LEN(2) | PAYLOAD| CRC16(2) |
+--------+--------+--------+--------+--------+----------+--------+
```

- SOF = `0xAA 0x55`
- CRC16/MODBUS：init `0xFFFF`，poly `0xA001` 反射，自检向量 `crc16("123456789") = 0x4B37`
- CRC 覆盖 `TYPE..PAYLOAD`，不含 SOF
- `SEQ` 与 `LEN` 均为小端

帧类型：

| TYPE | 方向 | 说明 |
|---|---|---|
| 0x01 | ESP → STM | `OTA_START` |
| 0x02 | ESP → STM | `OTA_DATA`，payload 为固件原始分片 |
| 0x03 | ESP → STM | `OTA_FINISH`，payload 为 uint32 CRC32 |
| 0x04 | ESP → STM | `OTA_ABORT`，payload 为 uint8 原因码 |
| 0x05 | ESP → STM | `LINK_STATUS`，无需 ACK |
| 0x10 | STM → ESP | STM 数据 JSON |
| 0x11 | STM → ESP | STM 状态 JSON |
| 0x81 | STM → ESP | ACK |
| 0x82 | STM → ESP | CRC_RESULT |

`OTA_START` payload（packed）：

```c
uint32_t total_size;   // 固件总字节数
uint16_t chunk_size;   // 分片大小
uint32_t crc32;        // 整包 CRC32，ESP 侧发送时传 0
uint8_t  version_len;  // 后随 version_len 字节版本号
```

`ACK` payload：

```c
uint16_t ack_seq;
uint8_t  status;       // 0=OK 1=CRC错误 2=Flash错误 3=序号错误
```

`LINK_STATUS` payload（5 字节）：

```c
uint8_t wifi;      // 1 = WiFi 已连接
uint8_t mqtt;      // 1 = MQTT 已连接
int8_t  rssi;      // dBm
uint8_t devices;   // 在线设备数
uint8_t ota_ready; // 1 = OTA 就绪
```

OTA 时序：

```text
ESP32                                      STM32
  |------ OTA_START(seq=0) ---------------->|
  |<----- ACK(seq=0, OK) -------------------|
  |------ OTA_DATA(seq=0, 1024B) ---------->|
  |<----- ACK(seq=0) -----------------------|
  |------ OTA_DATA(seq=1, 1024B) ---------->|
  |<----- ACK(seq=1) -----------------------|
  |                  ...                    |
  |------ OTA_FINISH(crc32) --------------->|
  |<----- CRC_RESULT(0) --------------------|
```

- 单帧 ACK 超时 500 ms，最多重传 3 次。
- 分片序号为 uint16，超出 65535 片的固件会被拒绝。
- 两片之间超过 3 秒无数据，Bootloader 判定链路超时并丢弃半成品。

### 4. 固件存储布局

STM32 内部 Flash：

| 区域 | 起始地址 | 大小 | 用途 |
|---|---|---|---|
| Bootloader | `0x08000000` | 64 KB | stm_ota |
| App | `0x08010000` | 960 KB | stm_modbus |

W25Q64（8 MB）：

| 偏移 | 用途 |
|---|---|
| `0x000000` | OTA 头（magic/state/size/crc/version/header CRC） |
| `0x001000` | 固件原始二进制，与云端 bin 逐字节一致 |

OTA 头状态机：

```text
EMPTY -> RECEIVING -> READY -> CONSUMED
```

只有整包 CRC32 校验通过才会置 `READY`，搬运阶段只接受 `READY`，因此任意时刻掉电最坏结果是重收一次，不会留下半个 App。

### 5. ESP32 分区表

`partitions.csv`：

| 分区 | 类型 | 大小 |
|---|---|---|
| nvs | data/nvs | 0x4000 |
| otadata | data/ota | 0x2000 |
| phy_init | data/phy | 0x1000 |
| factory | app/factory | 3 MB |
| ota_0 | app/ota_0 | 1 MB |
| ota_1 | app/ota_1 | 1 MB |

---

## 运行效果

### 界面

界面采用 240×320 竖屏布局：

- 顶部：标题栏、WiFi 图标、"已连接 / 未连接"。
- 中部：两张上下堆叠的传感器卡片，分别显示传感器 01/02 的温度和湿度。
- 底部：设备在线状态、WiFi RSSI、OTA 就绪指示灯。

界面参考稿见 [`lv_st7789_example/download.png`](lv_st7789_example/download.png)。该图是 320×240 横屏参考，实际屏幕为 240×320 竖屏，版式已按竖屏重排；实物截图待补充。

### 示例日志（ESP32 串口）

```text
I (1024) app: ESP32S3 OneNET MQTT + OTA 桥接启动，固件版本 1.0.0
I (2150) get_wifi_ota: WiFi 已启动，正在连接 "YOUR_SSID"
I (4380) get_wifi_ota: 获取到 IP: 192.168.1.23
I (5500) app: SNTP 对时成功: Sun Oct 18 21:30:00 2026
I (5610) onenet_mqtt: token 已刷新（有效期 2592000 秒）
I (5620) onenet_mqtt: 连接 mqtt://mqtts.heclouds.com:1883
I (6120) onenet_mqtt: 已连接到 OneNET
I (6121) onenet_mqtt: 上报属性: $sys/xxx/esp_1/thing/property/post
I (6122) onenet_mqtt: 订阅设置: $sys/xxx/esp_1/thing/property/set
I (6200) connect_stm: uart1 ready: tx=17 rx=18 baud=115200 chunk=1024
I (7000) app: 初始化完成，每 30 秒上报一次设备状态
I (20000) onenet_mqtt: 收到下行 [$sys/xxx/esp_1/thing/property/set] {"id":"1","params":{"url":{"value":"https://.../stm_modbus.bin"},"version":{"value":"1.0.1"},"size":{"value":365172},"md5":{"value":"..."},"target":{"value":"stm32"}}}
I (20001) app: 收到 OTA 任务: target=STM32 version=1.0.1 size=365172
I (20100) get_wifi_ota: 固件大小 365172 字节
I (20200) connect_stm: OTA_START acked: size=365172 chunk=1024 crc=0x00000000 ver=1.0.1
I (21000) get_wifi_ota: MD5 校验通过
I (21100) connect_stm: OTA_FINISH verified by STM32, crc=0x12345678
I (21101) get_wifi_ota: OTA 任务成功
```

### 构建结果（本地 Debug 构建示例）

| 固件 | text | data | bss | 说明 |
|---|---:|---:|---:|---|
| `stm_modbus.elf` | 364,932 B | 240 B | 63,136 B | 约 356 KB Flash / 63 KB RAM |
| `stm_ota.elf` | 27,472 B | 44 B | 27,900 B | 约 27 KB，满足 64 KB Bootloader 区 |
| `esp_mqtt_ota_connect.bin` | - | - | - | 1,061,744 B（Debug） |

`stm_modbus` 在 1 MB Flash 中占用约 356 KB，距离 960 KB 的 App 区上限仍有充足余量。

---

## 常见问题

### 编译与构建

**1. `idf.py` 报错 "MSys/Mingw is no longer supported"**

不要在 Git Bash / MSYS 中运行 `idf.py`。请使用 Windows CMD 的 ESP-IDF 命令行环境。

**2. CMake 报 generator 不匹配：Ninja vs MinGW Makefiles**

PATH 中存在其他 CMake（例如 STM32CubeCLT 自带的 3.28.1）时，`idf.py` 可能拿到错误的 CMake。确保 ESP-IDF 的 `cmake\3.30.2\bin` 和 `ninja` 位于 PATH 最前，或直接使用 ESP-IDF 快捷方式。已经污染时删除 `build/CMakeCache.txt` 或整个 `build/` 后重建。

**3. STM32 报 "找不到编译器"**

`arm-none-eabi-gcc` 不在 PATH。把 CubeCLT 的 `GNU-tools-for-STM32/bin`、`CMake/bin`、`Ninja/bin` 前置到 PATH。

**4. CubeMX 重新生成后 W25Q64 读不到 ID**

检查 `stm_ota.ioc` 中 SPI1 是否仍为 `Full-Duplex Master`，且 `PA6` 已分配为 `SPI1_MISO`。如果被改回 `TX_Only` / 单线模式，MISO 收不到数据，JEDEC ID 会读出 `0x000000` 或 `0xFFFFFF`。

**5. CubeMX 重新生成后一上电就反复复位**

`stm_modbus.ioc` 中的 WWDG 参数（DIV1 / Window=64 / Counter=64）在 42 MHz PCLK1 下只有约 97 µs 的刷新窗口。当前 `main.c` 已注释掉 `MX_WWDG_Init()`；重新生成后要么再次注释，要么从 `.ioc` 中彻底删除 WWDG。

### 网络与 MQTT

**6. MQTT 连接返回 `return_code=4`**

OneNET broker 对错误三元组统一返回 4（bad username or password），无法区分是产品不存在、密钥错误还是实例不匹配。先检查 ProductKey / DeviceName / DeviceSecret，并确认设备创建在控制台指定的实例上。Token 依赖时间，时间未同步也会导致鉴权失败。

**7. 属性上报被拒，code 2402**

OneNET 要求 `params` 中每个属性写成 `{"value": ...}`。本项目已在 `onenet_mqtt.c` 中统一包裹；如果自行拼接报文，注意不要使用扁平写法。

**8. MQTT 未连接时属性上报失败**

这是预期行为。WiFi 断开或 MQTT 重连期间，上报会返回 `ESP_ERR_INVALID_STATE`，底层会自动重连，恢复后继续上报。

### Modbus 与显示

**9. 传感器一直显示 "--"**

检查：

- RS485 A/B 是否接反，是否共地。
- 从站地址是否为 0x01 / 0x02。
- 波特率是否为 9600、8N1。
- 传感器寄存器是否确实为 FC 0x04、起始 0x0000、2 个寄存器。
- 使用自动换向模块时，`RS485_DE_ENABLE` 应为 0；使用 MAX3485 时应为 1 并接好 PD4。

**10. LCD 白屏或颜色异常**

- 白屏：先检查 CS/DC/RST/BL 宏与实际接线是否一致。可临时把 `LCD_BRINGUP_TEST` 置 1，观察红/绿/蓝/白/黑是否依次显示。
- 红蓝互换：调整 `LCD_MADCTL_BGR`（0x08 / 0x00）。
- 负片效果：检查 ST7789 初始化中的 `0x21`（INVON）/ `0x20`（INVOFF）。
- 花屏：检查 `LCD_COLOR_SWAP_PIXEL` 高低字节交换设置。

**11. 中文显示为空白**

`lv_st7789_example` 与 `stm_modbus` 使用的字体源码是压缩位图。必须确保 `stm_modbus/Middlewares/lvgl/lv_conf.h` 中 `LV_USE_FONT_COMPRESSED` 为 1，并与模拟器侧的字体生成参数一致。

### OTA

**12. OTA 任务下发后没有反应**

- 确认属性名 `url` 已在物模型中定义，且下发的 topic 是 `thing/property/set`。
- 确认 STM32 已经进入 Bootloader 下载模式。当前 App 未实现写 BKP0R + 复位的触发逻辑，可先用 `OTA_BOOT_WAIT_MS > 0` 的台架模式验证。
- 确认 `target` 为 `stm32`（省略时默认 stm32）。

**13. 下载失败，HTTP 状态码不是 200**

固件 URL 必须是静态直链，不能是网页、网盘分享页或需要登录的地址。确认 `Content-Length` 存在，URL 长度不超过 255 字节。

**14. MD5 或长度不匹配**

用同一份 bin 重新计算：

```bash
md5sum stm_modbus.bin
stat -c %s stm_modbus.bin
```

下发时填写的 `size` 和 `md5` 必须与文件完全一致。md5 使用 32 位十六进制小写字符串。

**15. ESP32 自升级报镜像过大**

当前 Debug 固件约 1.06 MB，而 `ota_0` / `ota_1` 各只有 1 MB。缩小固件（例如 Release 构建）或修改 `partitions.csv` 扩大 OTA 分区；修改分区表后必须重新烧录 bootloader 和分区表，OTA 本身不会更新分区表。

**16. STM32 升级后不启动**

首先检查 `stm_modbus` 的链接脚本是否已改为 `ORIGIN = 0x08010000`。其次确认下载的是裸 bin，而不是 hex、elf 或压缩包。

---

## 注意事项与限制

1. **STM32 OTA 触发链路尚未闭环。** Bootloader 已实现等待 `OTA_START` 并通过 RTC `BKP0R` 魔数判断升级请求，但 `stm_modbus` 应用尚未实现"收到云端 OTA 任务后写 `BKP0R` 并复位"的逻辑。目前只能通过 `OTA_BOOT_WAIT_MS > 0` 的台架模式或手动触发复位进入下载模式。
2. **STM32 应用链接地址需要迁移。** 当前链接脚本仍是 `0x08000000`，启用 OTA 前必须改为 `0x08010000` 并重新验证。
3. **ESP32 自升级分区不足。** Debug 固件约 1.06 MB，超过 1 MB 的 `ota_0` / `ota_1`。需要调整分区表或缩减固件。
4. **STM32 传感器数据尚未上传云端。** ESP32 已实现 `STM_DATA` / `STM_STATUS` 帧转发，但 STM32 应用目前只接收 `LINK_STATUS`，没有主动发送传感器 JSON。云端看到的 `wifi_rssi` / `stm_link` 来自 ESP32 本身。
5. **固件源没有访问控制。** 当前通过明文 URL 下载，只做 MD5/CRC32 完整性校验，没有固件签名。URL 泄露或服务器被篡改时，攻击者可以下发任意固件。量产时建议使用 HTTPS + 私有 OSS + 短期签名 URL，并增加固件数字签名与安全启动。
6. **凭据以明文存在 `sdkconfig`。** WiFi 密码和 OneNET DeviceSecret 会写进 `sdkconfig` 与构建产物。不要把 `sdkconfig` 提交到公开仓库；量产时应改为 NVS/provisioning 或安全分区存储。
7. **LCD 控制引脚仍是占位值。** `lcd_st7789.h` 中的 CS=PB0、DC=PB12、RST=PB11、BL=PB13 只是当前占位，必须按实际硬件修改。
8. **`stm_modbus.ioc` 与代码不同步。** USART2、LCD 控制脚、W25Q64 的 MISO/CS 均在组件代码中手工配置；重新生成 CubeMX 时需要注意不要破坏这些配置。
9. **WWDG 未启用。** 当前 `MX_WWDG_Init()` 被注释，系统没有硬件看门狗保护。需要先修正 WWDG 参数再启用。
10. **OTA 吞吐受串口波特率限制。** 115200 波特率下传输 1 MB 固件约需 90 秒。可根据可靠性测试结果提高到 921600，但必须同步修改 STM32 侧配置。
11. **一次只允许一个 OTA 任务。** OTA 任务忙时新的下发会被拒绝。
12. **WiFi 仅支持 2.4 GHz。** ESP32-S3 不支持 5 GHz。
13. **中文字体授权。** 内置字体由 Windows 等线/Deng 字体生成，商用发布前请确认字体授权，建议改用开源字体（如思源黑体）重新生成。
14. **原始方案文档标题中的"阿里云"是历史遗留。** 实际代码面向 OneNET，topic 使用 `$sys/...` 格式。

---

## 后续计划

- [ ] 在 `stm_modbus` 中实现收到 OTA 任务后写 `RTC->BKP0R = 0x4F544131` 并复位的逻辑，补齐远程 OTA 闭环。
- [ ] 将 `stm_modbus` 链接地址迁移到 `0x08010000`，并加入 CMake 自动生成 bin 的构建步骤。
- [ ] 重排 ESP32 分区表（去掉 3 MB factory 或扩大 OTA 分区），使自升级真正可用。
- [ ] 实现 STM32 → ESP32 的传感器 JSON 上行（`STM_DATA`），并映射到 OneNET 属性。
- [ ] 增加固件数字签名（ECDSA/Ed25519）与安全启动，替代仅 MD5/CRC32 的完整性校验。
- [ ] 增加 OTA 断点续传、失败回滚和版本回退能力。
- [ ] 将 WiFi / OneNET 凭据迁移到 NVS 或配网流程，避免明文写入 `sdkconfig`。
- [ ] 增加 Modbus 从站地址、寄存器映射、波特率的运行时配置。
- [ ] 增加设备端 OTA 进度界面和更完整的错误码展示。
- [ ] 补充实物照片、接线图和演示视频。
- [ ] 添加 GitHub Actions 或本地脚本，完成 ESP32 + STM32 双端自动构建。
- [ ] 统一 UI 字体为开源字体，消除字体授权风险。

---

## 许可证与联系方式

### 开源协议

当前仓库**尚未包含 LICENSE 文件**。如果计划开源，建议本项目采用 **MIT License**，并在发布前补充 `LICENSE` 文件。

第三方组件许可：

| 组件 | 许可证 |
|---|---|
| ESP-IDF | Apache-2.0 |
| STM32 HAL | BSD-3-Clause |
| LVGL | MIT |
| cJSON | MIT |

使用或分发时请同时保留第三方组件的版权与许可声明。

### 作者与维护者

- 作者 / 维护者：`weilipan`
- 邮箱：`weili31415@foxmail.com`
- 项目仓库：`<待填写>`
- Issue 反馈：请在项目仓库中提交 Issue，并附上：

  - 使用的硬件型号与接线
  - ESP-IDF / CubeCLT / LVGL 版本
  - 完整串口日志
  - 复现步骤与预期行为

---

## 参考

- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/)
- [LVGL 文档](https://docs.lvgl.io/8.3/)
- [OneNET 文档中心](https://open.iot.10086.cn/doc/)
- [Modbus 协议规范](https://modbus.org/specs.php)
