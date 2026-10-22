/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota_proto.h
  * @brief   stm_ota 的串口帧协议层 + CRC + USART1 DMA 接收。
  *
  *          协议与 ESP32S3 侧 components/connect_stm 逐字节对齐：
  *            帧 = AA 55 | TYPE(1) | SEQ(2 LE) | LEN(2 LE) | PAYLOAD | CRC16(2 LE)
  *            CRC16 = CRC16/MODBUS(init 0xFFFF, 反射, 表 0xA001)，
  *                    覆盖 TYPE..PAYLOAD（不含 SOF）。
  *
  *          接收为什么必须用 DMA 而不是字节中断：STM32F4 在擦/写内部 Flash
  *          期间总线停顿、CPU 取指被阻塞，中断在那段时间进不来；字节中断方式
  *          会丢数据。所以 USART1 RX 走 DMA2_Stream5/Channel4 环形缓冲，
  *          ISR 只负责把新到的字节搬进软件环形缓冲，解析全在主循环做。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __OTA_PROTO_H
#define __OTA_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ota_config.h"

/* 收到一个 CRC16 校验通过的完整帧时回调。
 * payload 指向内部缓冲，仅在回调期间有效；len 可能为 0。 */
typedef void (*ota_frame_cb_t)(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len);

/* ------------------------------------------------------------------ CRC */

/** CRC16/MODBUS，与 ESP32 侧 crc16_modbus() 完全一致。 */
uint16_t ota_crc16(const uint8_t *data, uint32_t len);

/**
 * @brief 标准 zlib CRC-32（反射，init 全 1，末取反），可流式累进。
 *
 * ESP32 侧用的是 esp_rom_crc32_le，从 0 起累进调用；两者的结果与
 * 标准 zlib crc32() 完全相同（见 esp_rom/linux/esp_rom_crc.c 的等价实现）。
 *
 * @param crc 上次调用的返回值；首次调用传 0
 * @param data 数据
 * @param len 长度
 * @return 新的 CRC 值（可直接与云端给的 crc32 比较）
 */
uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/** 一次性算整块的标准 CRC-32。 */
uint32_t ota_crc32(const uint8_t *data, uint32_t len);

/* ------------------------------------------------------------------ 初始化 */

/**
 * @brief 初始化 USART1 的 DMA 环形接收与帧解析器。
 *
 * 必须在 MX_USART1_UART_Init() 之后调用。同一个 huart1 上的阻塞发送
 * （HAL_UART_Transmit）与这里的 DMA 接收互不干扰，可以并存。
 *
 * @return true 表示 CRC32 自检通过（crc32("123456789") == 0xCBF43926）
 */
bool ota_proto_init(void);

/** 注册帧回调。 */
void ota_proto_register_rx(ota_frame_cb_t cb);

/* ------------------------------------------------------------------ 收发 */

/** 组帧并阻塞发送（用 huart1）。帧都 ≤170 字节，阻塞发送足够了。 */
bool ota_proto_send(uint8_t type, uint16_t seq, const void *payload, uint16_t len);

/** 应答 ACK，payload = {ack_seq(2 LE), status(1)}。 */
bool ota_proto_send_ack(uint16_t ack_seq, uint8_t status);

/** 上报整包校验结果，payload = {status(1)}。 */
bool ota_proto_send_crc_result(uint8_t status);

/**
 * @brief 上报一个 JSON 状态对象给 ESP32（会被原样转发到 OneNET）。
 *
 * 形如 {"ota_stage":"receiving","ota_percent":37,"msg":"..."}，
 * msg 传 NULL 则省略该字段。总长度受 OTA_STATUS_JSON_MAX 限制。
 */
bool ota_proto_status_json(const char *stage, int percent, const char *msg);

/* ------------------------------------------------------------------ 轮询 */

/**
 * @brief 主循环里反复调用：搬运 DMA 数据并解析帧。
 *
 * 解析出的完整帧在**本函数内部**同步回调，所以回调里可以做落盘等慢操作。
 */
void ota_proto_poll(void);

/** 丢弃软件环形缓冲里尚未处理的内容（链路出错后重同步用）。 */
void ota_proto_resync(void);

/**
 * @brief 取接收侧的诊断计数（联调时上报出去，一眼看出链路质量）。
 *
 * @param dropped 软件环形缓冲溢出而丢弃的字节数（正常应恒为 0）
 * @param bad_crc CRC16 不匹配的帧数（有值说明线上有噪声或波特率不对）
 * @param bad_len LEN 字段超限的帧数（有值说明严重失步）
 * @param errors  USART 错误标志次数（ORE/FE/NE/PE）
 */
void ota_proto_get_diag(uint32_t *dropped, uint32_t *bad_crc, uint32_t *bad_len,
                        uint32_t *errors);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_PROTO_H */
