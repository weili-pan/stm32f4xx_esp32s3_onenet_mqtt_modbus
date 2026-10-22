# STM32F407 onenet MQTT 与 WiFi OTA 项目方案

硬件组成、系统架构与实施路线

**项目定位** 本项目面向 Windows 开发环境，使用 STM32F407 作为主控、ESP32S3 作为 WiFi 与云端通信模块，通过onenet IoT MQTT 完成设备数据上报、远程控制和 OTA 升级。STM32F407负责采集与控制，ESP32S3负责网络、TLS、MQTT和HTTPS下载，从而降低主控处理云端通信的复杂度。

## 一 项目总体架构

```text
Modbus传感器/执行器
        │ RS485 / Modbus RTU
        ▼
STM32F407 ──UART── ESP32S3 ──WiFi + TLS + MQTT/HTTPS── onenet IoT
    │                         │
    ├── ST7789 + LVGL         └── OTA固件下载
    ├── W25Q64外部Flash
    └── Bootloader
```

## 二 推荐硬件清单

| 类别 | 推荐器件 | 数量 | 用途 |
| --- | --- | --- | --- |
| 主控 | STM32F407ZGT6/VGT6开发板 | 1 | Modbus采集、控制、LVGL和Bootloader |
| WiFi模块 | ESP32S3 | 1 | WiFi、onenetMQTT、TLS和HTTPS下载 |
| 外部存储 | W25Q64JV SPI Flash模块 | 1 | 保存待升级固件 |
| RS485 | MAX3485、SP3485或THVD1450模块 | 1～3 | 连接Modbus RTU设备 |
| 显示 | ST7789 SPI LCD | 1 | 显示数据、网络状态和OTA进度 |
| 调试下载 | ST-Link V2/V3 | 1 | 烧录和调试STM32 |
| 串口调试 | CP2102或CH340 USB-TTL | 1 | 查看ESP32S3和STM32日志 |
| 电源 | 稳定的5V输入和3.3V稳压模块 | 1 | 为系统提供可靠供电 |
| 测试设备 | Modbus传感器或串口Modbus模拟器 | 1 | 验证采集链路 |

## 三 硬件连接关系

### 3.1 STM32F407与ESP32S3

```text
STM32 USART_TX A9 → ESP32S3 RX 18
STM32 USART_RX A10 ← ESP32S3 TX 17
STM32 GND       ─ ESP32S3 GND
```

建议使用一个独立串口连接ESP32S3，例如 USART1；使用另一个串口连接RS485，例如 USART2。STM32与ESP32S3之间建议使用自定义二进制协议传输数据和OTA分片。

### 3.2 STM32F407与W25Q64

```text
STM32 SPI_SCK  PA5  → W25Q SCLK
STM32 SPI_MOSI PA7  → W25Q DI
STM32 SPI_MISO PA6  ← W25Q DO
STM32 GPIO     PB1  → W25Q CS
3.3V                → VCC
GND                 → GND
```

### 3.3 STM32F407与ST7789

```text
STM32 SPI_SCK  PB10 → ST7789 SCL
STM32 SPI_MOSI PC3  → ST7789 SDA
STM32 GPIO     PB0  → ST7789 CS
STM32 GPIO     PB12 → ST7789 DC
STM32 GPIO     PB11 → ST7789 RES
STM32 GPIO     PB13 → ST7789 BL
```

ST7789和W25Q64可以共用SPI总线，但必须使用不同的片选CS引脚。

### 3.4 STM32F407与RS485

```text
STM32 USART2_TX PA2 → RS485 DI
STM32 USART2_RX PA3 ← RS485 RO
STM32 GPIO      PD4 → RS485 DE/RE
RS485 A/B           → Modbus设备
```

## 四 onenet MQTT 通信设计

在onenet物联网平台创建产品和设备，获取以下设备信息：

| 参数 | 说明 |
| --- | --- |
| ProductKey | 产品标识 |
| DeviceName | 设备名称 |
| DeviceSecret | 设备密钥，不能公开或提交到GitHub |
| MQTT Endpoint | 以onenet控制台显示的实例地址为准 |

正式环境建议使用 TLS 端口 8883。MQTT用户名、密码和ClientID由设备三元组与签名参数生成，实际格式应以onenet控制台或官方SDK为准。

### 4.1 属性上报

`/sys/${ProductKey}/${DeviceName}/thing/event/property/post`

```json
{
  "id": "1001",
  "version": "1.0",
  "params": {
    "temperature": 26.5,
    "wifi_rssi": -55,
    "firmware_version": "1.0.0"
  },
  "method": "thing.event.property.post"
}
```

### 4.2 建议上报内容

- 传感器数据和设备运行参数
- Modbus设备在线状态和故障码
- WiFi信号强度与云端连接状态
- 当前固件版本
- OTA升级进度和升级结果

## 五 OTA 升级方案

推荐使用“MQTT负责通知、HTTPS负责下载、W25Q64负责缓存、Bootloader负责升级”的方案。MQTT不直接承载完整固件文件，能够减少大消息、断线重传和内存占用问题。

```text
固件编译 → 上传onenetOTA → MQTT通知设备 → ESP32S3 HTTPS下载
→ UART分片传给STM32 → W25Q64缓存 → CRC/SHA校验
→ 重启进入Bootloader → 更新应用 → MQTT上报结果
```

### 5.1 STM32F407 Flash规划

| 存储区域 | 内容 |
| --- | --- |
| 内部Flash 0x08000000起始区域 | Bootloader，建议预留约64 KB |
| 内部Flash 0x08010000起始区域 | Application用户应用程序 |
| W25Q64外部Flash | 下载中的新固件和升级状态 |

### 5.2 OTA必须实现的保护机制

- 校验固件大小、版本和目标设备型号
- 使用CRC32或SHA256验证完整性
- 升级过程中断电后不能破坏旧程序
- 新程序首次启动成功后再确认升级完成
- 升级失败时保留旧程序或进入安全模式
- 设备周期性上报下载进度和错误原因

## 六 STM32 与 ESP32S3 的 OTA 串口协议

```text
ESP32S3 → STM32：OTA_START
STM32 → ESP32S3：ACK
ESP32S3 → STM32：DATA(index, payload)
STM32 → ESP32S3：ACK(index)
ESP32S3 → STM32：OTA_FINISH
STM32 → ESP32S3：CRC_RESULT
```

建议每个分片使用1024或2048字节，并增加序号、长度和CRC16。STM32只有在数据成功写入W25Q64后才回复ACK，ESP32S3未收到ACK时进行重传。

## 七 软件模块划分

| 模块 | 主要内容 |
| --- | --- |
| STM32F407 | Bootloader、Modbus RTU、UART DMA、W25Q64、OTA状态机、LVGL、ST7789、FreeRTOS |
| ESP32S3 | WiFi连接、TLS、onenetMQTT、OTA通知解析、HTTPS固件下载、UART转发、断线重连 |
| Windows | STM32CubeIDE、ESP-IDF、STM32CubeProgrammer、MQTTX、Python测试脚本 |

## 八 推荐开发顺序

1. STM32F407与ESP32S3完成串口通信。
2. ESP32S3连接WiFi并实现onenetMQTT连接。
3. 实现设备属性上报和云端控制命令。
4. STM32完成RS485和Modbus RTU采集。
5. 加入ST7789和LVGL，显示传感器数据。
6. STM32完成W25Q64读写和固件校验。
7. 实现ESP32S3 HTTPS下载并通过UART发送固件。
8. 完成Bootloader更新、断电保护和升级结果上报。

## 九 最低可运行配置

第一版建议先购买以下器件，验证onenet通信和OTA，再扩展Modbus和显示功能：

- STM32F407开发板 1块
- ESP32S3开发板 1块
- W25Q64模块 1块
- ST-Link 1个
- USB-TTL 1个
- 稳定的3.3V电源模块 1个
- MAX3485模块和Modbus传感器各1个

## 十 GitHub参考项目

- FreeRTOS：https://github.com/FreeRTOS/FreeRTOS
- FreeRTOS coreMQTT：https://github.com/FreeRTOS/coreMQTT
- Paho Embedded C：https://github.com/eclipse-paho/paho.mqtt.embedded-c
- LVGL：https://github.com/lvgl/lvgl
- ESP-IDF：https://github.com/espressif/esp-idf
- FreeModbus：https://github.com/cwalter-at/freemodbus
- libmodbus：https://github.com/stephane/libmodbus

GitHub搜索关键词：STM32F407 bootloader OTA、STM32 W25Q64 OTA、STM32 ESP32S3 UART OTA、ESP32S3 Alibaba Cloud MQTT、STM32 Modbus MQTT gateway、STM32 LVGL ST7789。

## 十一 结论

推荐最终硬件组合为：STM32F407 + ESP32S3-WROOM-32 + W25Q64 + MAX3485 + ST7789 + onenetIoT。ESP32S3负责网络和云端安全通信，STM32F407负责工业采集、控制、界面和可靠升级。该架构适合先做原型，再扩展为多路Modbus工业网关。
