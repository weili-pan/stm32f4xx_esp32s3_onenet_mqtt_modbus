#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stm_ota 台架测试脚本：不用 ESP32S3、不用云端，直接用 PC 串口给 STM32 刷固件。

它把 ESP32S3 侧 components/connect_stm 的发送行为原样复刻了一遍
（同样的组帧、同样的 CRC16/MODBUS、同样的等 ACK + 重传策略），
所以这个脚本能跑通，就说明 STM32 侧的协议实现和 ESP32 侧是对齐的。

帧格式（小端）：
    AA 55 | TYPE(1) | SEQ(2) | LEN(2) | PAYLOAD | CRC16(2)
    CRC16 覆盖 TYPE..PAYLOAD，不含 SOF。

用法（用 ESP-IDF 自带的 python 跑，里面已经装了 pyserial）：

    python ota_send.py -p COM5 -f app.bin
    python ota_send.py -p COM5 -f app.bin -v 1.0.3 -c 1024
    python ota_send.py -p COM5 -f app.bin --corrupt 4096   # 故意改坏一个字节

注意：要让板子进下载模式，需要 OTA_BOOT_WAIT_MS > 0 重编一次（台架用），
或者让 App 写 RTC->BKP0R 魔数后复位。
"""

import argparse
import queue
import struct
import sys
import threading
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial：用 IDF 的 python 跑，或先 pip install pyserial")

SOF0 = 0xAA
SOF1 = 0x55
HEADER_LEN = 7
CRC_LEN = 2

FRAME_OTA_START = 0x01
FRAME_OTA_DATA = 0x02
FRAME_OTA_FINISH = 0x03
FRAME_OTA_ABORT = 0x04
FRAME_STM_DATA = 0x10
FRAME_STM_STATUS = 0x11
FRAME_ACK = 0x81
FRAME_CRC_RESULT = 0x82

FRAME_NAMES = {
    FRAME_OTA_START: "OTA_START",
    FRAME_OTA_DATA: "OTA_DATA",
    FRAME_OTA_FINISH: "OTA_FINISH",
    FRAME_OTA_ABORT: "OTA_ABORT",
    FRAME_STM_DATA: "STM_DATA",
    FRAME_STM_STATUS: "STM_STATUS",
    FRAME_ACK: "ACK",
    FRAME_CRC_RESULT: "CRC_RESULT",
}

ACK_OK = 0x00
ACK_CRC_ERROR = 0x01
ACK_FLASH_ERR = 0x02
ACK_OUT_OF_SEQ = 0x03
ACK_NAMES = {
    ACK_OK: "OK",
    ACK_CRC_ERROR: "CRC_ERROR",
    ACK_FLASH_ERR: "FLASH_ERR",
    ACK_OUT_OF_SEQ: "OUT_OF_SEQ",
}

ACK_TIMEOUT_S = 0.5        # 与 ESP32 侧 CONNECT_STM_ACK_TIMEOUT_MS 一致
FINISH_TIMEOUT_S = 2.0     # 与 ESP32 侧 ACK_TIMEOUT_MS * 4 一致
MAX_RETRY = 3              # 与 ESP32 侧 CONNECT_STM_MAX_RETRY 一致


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

def build_frame(ftype, seq, payload=b""):
    head = struct.pack("<BHH", ftype, seq & 0xFFFF, len(payload))
    body = head + payload
    crc = crc16_modbus(body)
    return bytes([SOF0, SOF1]) + body + struct.pack("<H", crc)


def send_frame(ser, ftype, seq, payload=b"", quiet=False):
    frame = build_frame(ftype, seq, payload)
    ser.write(frame)
    ser.flush()
    if not quiet:
        print(f"  -> {FRAME_NAMES.get(ftype, hex(ftype))} seq={seq} len={len(payload)}")


# ---------------------------------------------------------------- 接收线程

class Receiver(threading.Thread):
    """按 connect_stm.c 的 rx_task 一样的逻辑逐字节找 SOF 并解析帧。"""

    def __init__(self, ser):
        super().__init__(daemon=True)
        self.ser = ser
        self.events = queue.Queue()
        self.stop = threading.Event()
        self.resync = 0

    def run(self):
        st = "sof0"
        hdr = bytearray()
        payload = bytearray()
        need = 0
        crc = bytearray()

        while not self.stop.is_set():
            b = self.ser.read(1)
            if not b:
                continue
            b = b[0]

            if st == "sof0":
                if b == SOF0:
                    st = "sof1"
            elif st == "sof1":
                if b == SOF1:
                    hdr = bytearray()
                    st = "hdr"
                elif b != SOF0:
                    st = "sof0"
            elif st == "hdr":
                hdr.append(b)
                if len(hdr) == HEADER_LEN - 2:
                    ftype, seq, ln = struct.unpack("<BHH", bytes(hdr))
                    need = ln
                    payload = bytearray()
                    crc = bytearray()
                    st = "payload" if ln else "crc"
            elif st == "payload":
                payload.append(b)
                if len(payload) == need:
                    st = "crc"
            elif st == "crc":
                crc.append(b)
                if len(crc) == CRC_LEN:
                    cover = bytes(hdr) + bytes(payload)
                    if crc16_modbus(cover) == struct.unpack("<H", bytes(crc))[0]:
                        self.events.put((ftype, seq, bytes(payload)))
                    else:
                        self.resync += 1
                        print("  !! CRC16 不匹配，丢弃一帧并重同步")
                    st = "sof0"

    def wait_for(self, want_type, want_seq=None, timeout=ACK_TIMEOUT_S):
        """等指定类型的帧。返回 (ftype, seq, payload) 或 None（超时）。"""
        deadline = time.monotonic() + timeout
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return None
            try:
                ftype, seq, payload = self.events.get(timeout=remain)
            except queue.Empty:
                return None
            self._report(ftype, seq, payload)
            if ftype == want_type and (want_seq is None or seq == want_seq):
                return (ftype, seq, payload)


def print_status(payload):
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        print(f"  <- STM_STATUS (非 UTF-8，{len(payload)} 字节)")
        return
    print(f"  <- STM_STATUS {text}")


# 给 Receiver 挂上「收到非目标帧时的打印」行为
def _report(self, ftype, seq, payload):
    if ftype in (FRAME_STM_STATUS, FRAME_STM_DATA):
        print_status(payload)
    elif ftype == FRAME_ACK:
        ack_seq, status = struct.unpack("<HB", payload[:3])
        print(f"  <- ACK ack_seq={ack_seq} status={ACK_NAMES.get(status, status)}")
    elif ftype == FRAME_CRC_RESULT:
        print(f"  <- CRC_RESULT status={payload[0] if payload else '?'}")
    else:
        print(f"  <- {FRAME_NAMES.get(ftype, hex(ftype))} seq={seq} len={len(payload)}")


Receiver._report = _report


# ---------------------------------------------------------------- 主流程

def ota_begin(ser, rx, total, chunk, version):
    payload = struct.pack("<IHIB", total, chunk, 0, len(version)) + version
    for attempt in range(1, MAX_RETRY + 1):
        send_frame(ser, FRAME_OTA_START, 0, payload)
        got = rx.wait_for(FRAME_ACK, 0, ACK_TIMEOUT_S)
        if got is not None:
            ack_seq, status = struct.unpack("<HB", got[2][:3])
            if status == ACK_OK:
                print(f"  OTA_START 已确认：size={total} chunk={chunk} ver={version}")
                return True
            print(f"  OTA_START 被拒：status={ACK_NAMES.get(status, status)}")
            return False
        print(f"  OTA_START 第 {attempt}/{MAX_RETRY} 次超时，重传")
    print("  OTA_START 彻底失败")
    return False


def ota_data(ser, rx, seq, buf):
    for attempt in range(1, MAX_RETRY + 1):
        send_frame(ser, FRAME_OTA_DATA, seq, buf, quiet=True)
        got = rx.wait_for(FRAME_ACK, seq, ACK_TIMEOUT_S)
        if got is not None:
            _, status = struct.unpack("<HB", got[2][:3])
            if status == ACK_OK:
                return True
            if status == ACK_OUT_OF_SEQ:
                # STM32 没收到期望的那一片。协议里没有「请重发第 N 片」的语义，
                # 重传当前片也不会被接受，所以这里直接判定本片失败。
                print(f"  分片 {seq}：STM32 报 OUT_OF_SEQ，本片无法继续")
                return False
            print(f"  分片 {seq}：ACK status={ACK_NAMES.get(status, status)}，重传")
        else:
            print(f"  分片 {seq} 第 {attempt}/{MAX_RETRY} 次超时，重传")
        time.sleep(0.01)   # 与 ESP32 侧一样，留点时间给 STM32 恢复
    return False


def ota_finish(ser, rx, crc):
    for attempt in range(1, MAX_RETRY + 1):
        send_frame(ser, FRAME_OTA_FINISH, 0, struct.pack("<I", crc))
        got = rx.wait_for(FRAME_CRC_RESULT, None, FINISH_TIMEOUT_S)
        if got is not None:
            status = got[2][0] if got[2] else 0xFF
            if status == ACK_OK:
                print(f"  STM32 整包 CRC32 校验通过（0x{crc:08X}）")
                return True
            print(f"  STM32 整包 CRC32 校验失败，status={status}")
            return False
        print(f"  OTA_FINISH 第 {attempt}/{MAX_RETRY} 次超时，重传")
    return False


def main():
    ap = argparse.ArgumentParser(description="stm_ota 台架烧录脚本（复刻 ESP32S3 发送侧）")
    ap.add_argument("-p", "--port", required=True, help="串口号，例如 COM5")
    ap.add_argument("-f", "--file", required=True, help="要烧的 .bin")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-c", "--chunk", type=int, default=1024, help="分片大小，默认 1024")
    ap.add_argument("-v", "--version", default="bench-test", help="版本号字符串")
    ap.add_argument("--corrupt", type=int, default=-1,
                    help="故意把第 N 个字节取反，用来验证 CRC 不一致路径")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="等待 STM32 进入等待态的秒数")
    args = ap.parse_args()

    data = bytearray(open(args.file, "rb").read())
    if args.corrupt >= 0:
        if args.corrupt >= len(data):
            sys.exit(f"--corrupt {args.corrupt} 超出文件长度 {len(data)}")
        data[args.corrupt] ^= 0xFF
        print(f"已改坏第 {args.corrupt} 个字节（0x{args.corrupt:04X}）")
    data = bytes(data)

    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"文件 {args.file}：{len(data)} 字节，本地 CRC32 = 0x{crc:08X}")

    if len(data) == 0:
        sys.exit("空文件没法烧")
    if len(data) % args.chunk != 0:
        print(f"提示：{len(data)} 不是分片大小 {args.chunk} 的整数倍，最后一片会短一些。")

    with serial.Serial(args.port, args.baud, timeout=0.02) as ser:
        ser.reset_input_buffer()
        rx = Receiver(ser)
        rx.start()

        print(f"打开 {args.port} @ {args.baud}，等 STM32 进入等待态……")
        # STM32 进下载模式后会周期性发 ready 状态；看到它（或超时）就开始发
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            try:
                ftype, seq, payload = rx.events.get(timeout=0.2)
            except queue.Empty:
                continue
            rx._report(ftype, seq, payload)
            if ftype in (FRAME_STM_STATUS, FRAME_STM_DATA):
                break

        if not ota_begin(ser, rx, len(data), args.chunk, args.version.encode()):
            sys.exit("OTA_START 失败，退出")

        total_chunks = (len(data) + args.chunk - 1) // args.chunk
        t0 = time.monotonic()
        for i in range(total_chunks):
            off = i * args.chunk
            if not ota_data(ser, rx, i, data[off:off + args.chunk]):
                sys.exit(f"分片 {i} 失败，退出")
            done = (i + 1) * 100 // total_chunks
            print(f"\r  分片 {i + 1}/{total_chunks}  {done}%", end="", flush=True)
        print()

        elapsed = time.monotonic() - t0
        print(f"  传输完成，{len(data)} 字节用时 {elapsed:.1f}s "
              f"（{len(data) / elapsed / 1024:.1f} KB/s）")

        ok = ota_finish(ser, rx, crc)

        # 搬运阶段 STM32 会继续上报 installing 进度，这里陪它跑一会
        print("等 STM32 搬运并校验（进度会以 STM_STATUS 打出来）……")
        deadline = time.monotonic() + 120.0
        while time.monotonic() < deadline:
            try:
                ftype, seq, payload = rx.events.get(timeout=1.0)
            except queue.Empty:
                break
            rx._report(ftype, seq, payload)

        rx.stop.set()
        print(f"\n结论：{'协议传输与整包校验通过' if ok else '整包校验未通过'}"
              f"；接收侧重同步 {rx.resync} 次")


if __name__ == "__main__":
    main()
