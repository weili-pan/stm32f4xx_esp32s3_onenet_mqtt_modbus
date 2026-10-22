#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stm_modbus 台架测试脚本：不用 ESP32S3，直接用 PC 串口给 STM32 发 LINK_STATUS 帧。

它把 ESP32S3 侧 connect_stm_send_link_status() 的行为原样复刻了一遍（同样的组帧、
同样的 CRC16/MODBUS），所以屏幕跟着变，就说明 STM32 侧的收帧+解析+刷 UI 这条链是通的；
屏幕不动，那就是 STM32 侧（收帧/解析）或接线的问题，跟 ESP32 无关。

帧格式（小端）：
    AA 55 | TYPE(1) | SEQ(2) | LEN(2) | PAYLOAD | CRC16(2)
    TYPE = 0x05，PAYLOAD = wifi, mqtt, rssi(int8), devices, ota_ready（各 1 字节）

接线（把 ESP32 从 PA9/PA10 上摘掉，换成 USB-TTL）：
    USB-TTL TX  -> STM32 PA10 (USART1_RX)
    USB-TTL RX  -> STM32 PA9  (USART1_TX)   # 只发不收的话可以不接
    GND         -> GND

用法（用 ESP-IDF 自带的 python 跑，里面已经装了 pyserial）：
    硬件可见的判据：板载 LED1(PC13) 亮着 = STM32 在收帧（在线），灭 = 3s 没收到（离线）。

    python send_status.py -p COM5                          # 一直发：wifi/mqtt 在线, rssi=-55
    python send_status.py -p COM5 --rssi -80               # 换个信号强度看「WiFi -80 dBm」
    python send_status.py -p COM5 --toggle 5               # 每 5 秒在「已连接/未连接」之间切
    python send_status.py -p COM5 --offline                # 发一次全 0（模拟断开），然后停
    python send_status.py -p COM5 --duration 15            # 发 15 秒后停，看 3s 后 UI 是否回落
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial：用 IDF 的 python 跑，或先 pip install pyserial")

SOF0 = 0xAA
SOF1 = 0x55
FRAME_LINK_STATUS = 0x05


# ---------------------------------------------------------------- CRC16/MODBUS

def _build_crc16_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        table.append(c)
    return table


CRC16_TABLE = _build_crc16_table()


def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc = (crc >> 8) ^ CRC16_TABLE[(crc ^ b) & 0xFF]
    return crc


assert crc16_modbus(b"123456789") == 0x4B37, "CRC16 表自检失败"


# ---------------------------------------------------------------- 组帧 / 发帧

def build_link_status(wifi, mqtt, rssi, devices, ota_ready):
    payload = struct.pack("<BBBbB", wifi & 1, mqtt & 1, rssi, devices & 0xFF, ota_ready & 1)
    body = struct.pack("<BHH", FRAME_LINK_STATUS, 0, len(payload)) + payload
    return bytes([SOF0, SOF1]) + body + struct.pack("<H", crc16_modbus(body))


def main():
    ap = argparse.ArgumentParser(description="给 STM32 灌 LINK_STATUS 帧（台架用）")
    ap.add_argument("-p", "--port", required=True, help="STM32 侧串口，如 COM5")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=1.0, help="发送间隔秒（默认 1.0，同 ESP32）")
    ap.add_argument("--duration", type=float, default=0, help="发多久后停（秒，0 = 一直发）")
    ap.add_argument("--rssi", type=int, default=-55, help="信号强度 dBm（默认 -55）")
    ap.add_argument("--devices", type=int, default=1, help="在线设备数（默认 1）")
    ap.add_argument("--offline", action="store_true", help="发一帧全 0（wifi/mqtt/devices/ota 全断）后退出")
    ap.add_argument("--toggle", type=float, default=0, help="每 N 秒在 已连接/未连接 之间切换（0 = 不切）")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print(f"打开 {args.port} @ {args.baud}，Ctrl+C 退出")
    print("判据：板载 LED1(PC13) 亮 = STM32 在收帧；灭 = 3 秒没收到")

    def send(wifi, mqtt, rssi, devices, ota):
        frame = build_link_status(wifi, mqtt, rssi, devices, ota)
        ser.write(frame)
        print(f"  -> 0x05 wifi={wifi} mqtt={mqtt} rssi={rssi} devices={devices} ota={ota}"
              f"  [{frame.hex(' ')}]")

    try:
        if args.offline:
            send(0, 0, 0, 0, 0)
            return

        t0 = time.time()
        wifi = 1
        last_toggle = t0
        while True:
            now = time.time()
            if args.duration and (now - t0) >= args.duration:
                print("到时，停止发送（3 秒后屏幕应回落到未连接）")
                break
            if args.toggle and (now - last_toggle) >= args.toggle:
                wifi = 0 if wifi else 1
                last_toggle = now
            send(wifi, wifi, args.rssi if wifi else 0, args.devices if wifi else 0, wifi)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n中断退出")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
