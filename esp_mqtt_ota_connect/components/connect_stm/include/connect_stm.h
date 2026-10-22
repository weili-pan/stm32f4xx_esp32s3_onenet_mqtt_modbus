/*
 * connect_stm —— ESP32S3 与 STM32F407 之间的串口二进制帧协议
 *
 * 帧格式（小端）：
 *   +--------+--------+--------+--------+--------+----------+--------+
 *   | SOF(2) | TYPE(1)| SEQ(2) | LEN(2) | PAYLOAD| CRC16(2) |
 *   +--------+--------+--------+--------+--------+----------+--------+
 *   SOF = 0xAA 0x55，CRC16 为 CRC16/MODBUS，覆盖 TYPE..PAYLOAD
 *
 * OTA 数据帧的序号直接使用帧头 SEQ，因此 OTA_DATA 的 payload 只放原始固件数据。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONNECT_STM_SOF_0 0xAA
#define CONNECT_STM_SOF_1 0x55
#define CONNECT_STM_HEADER_LEN 7  /* SOF(2) + TYPE(1) + SEQ(2) + LEN(2) */
#define CONNECT_STM_CRC_LEN 2

/* 帧类型：0x01-0x0F 为 ESP32S3 → STM32；0x10-0x7F 为 STM32 主动上报；0x80-0xFF 为 STM32 应答 */
typedef enum {
    CONNECT_STM_FRAME_OTA_START  = 0x01, /* ESP→STM: ota_start_payload_t */
    CONNECT_STM_FRAME_OTA_DATA   = 0x02, /* ESP→STM: 原始固件分片 */
    CONNECT_STM_FRAME_OTA_FINISH = 0x03, /* ESP→STM: uint32_t crc32 */
    CONNECT_STM_FRAME_OTA_ABORT  = 0x04, /* ESP→STM: uint8_t reason */

    CONNECT_STM_FRAME_LINK_STATUS = 0x05, /* ESP→STM: connect_stm_link_status_t，无需 ACK */

    CONNECT_STM_FRAME_ACK        = 0x81, /* STM→ESP: ack_payload_t */
    CONNECT_STM_FRAME_CRC_RESULT = 0x82, /* STM→ESP: uint8_t status */
    CONNECT_STM_FRAME_STM_DATA   = 0x10, /* STM→ESP: JSON 文本 */
    CONNECT_STM_FRAME_STM_STATUS = 0x11, /* STM→ESP: JSON 文本 */
} connect_stm_frame_type_t;

/* ACK 状态码 */
typedef enum {
    CONNECT_STM_ACK_OK         = 0x00,
    CONNECT_STM_ACK_CRC_ERROR  = 0x01,
    CONNECT_STM_ACK_FLASH_ERR  = 0x02,
    CONNECT_STM_ACK_OUT_OF_SEQ = 0x03,
} connect_stm_ack_status_t;

/* OTA 中止原因 */
typedef enum {
    CONNECT_STM_ABORT_DOWNLOAD_FAIL = 0x01,
    CONNECT_STM_ABORT_SIZE_MISMATCH = 0x02,
    CONNECT_STM_ABORT_CRC_MISMATCH  = 0x03,
    CONNECT_STM_ABORT_TIMEOUT       = 0x04,
} connect_stm_abort_reason_t;

/* OTA_START 帧负载 */
typedef struct __attribute__((packed)) {
    uint32_t total_size;   /* 固件总字节数 */
    uint16_t chunk_size;   /* 分片大小，用于 STM32 侧预分配缓冲 */
    uint32_t crc32;        /* 整包 CRC32，便于 STM32 提前校验元数据 */
    uint8_t  version_len;  /* 后随 version_len 字节版本号，无终止符 */
} connect_stm_ota_start_t;

/* ACK 帧负载 */
typedef struct __attribute__((packed)) {
    uint16_t ack_seq;      /* 被确认的帧序号 */
    uint8_t  status;       /* connect_stm_ack_status_t */
} connect_stm_ack_payload_t;

/* LINK_STATUS 帧负载：STM32 侧照抄同名字段（见 stm_modbus/Components/Link/esp_link.h） */
typedef struct __attribute__((packed)) {
    uint8_t wifi;      /* 1 = WiFi 已连接 */
    uint8_t mqtt;      /* 1 = OneNET MQTT 已连接 */
    int8_t  rssi;      /* WiFi 信号强度 dBm，如 -55 */
    uint8_t devices;   /* 在线设备数（ESP32 视角的 STM32 链路：在线为 1） */
    uint8_t ota_ready; /* 1 = OTA 就绪（可接收下发任务） */
} connect_stm_link_status_t;

typedef struct {
    const uint8_t *data;   /* payload 指针，仅在回调内有效 */
    uint16_t len;
} connect_stm_frame_t;

/* 收到 STM32 上行帧时回调（在 connect_stm 的 RX 任务上下文中执行，勿做阻塞操作） */
typedef void (*connect_stm_rx_cb_t)(uint8_t type, uint16_t seq, const connect_stm_frame_t *payload);

/**
 * @brief 初始化 UART 并启动接收任务
 */
esp_err_t connect_stm_init(void);

/**
 * @brief 注册上行帧回调，传 NULL 取消注册
 */
void connect_stm_register_rx_cb(connect_stm_rx_cb_t cb);

/**
 * @brief 发送任意类型的帧
 */
esp_err_t connect_stm_send_frame(uint8_t type, uint16_t seq, const void *payload, uint16_t len);

/**
 * @brief 发送 OTA_START 并等待 ACK
 */
esp_err_t connect_stm_ota_begin(uint32_t total_size, uint16_t chunk_size, uint32_t crc32,
                                const char *version);

/**
 * @brief 发送一个固件分片并等待 ACK，超时会自动重传
 *
 * @param seq 分片序号，从 0 递增
 * @param buf 分片数据
 * @param len 分片长度，不得超过 CONFIG_CONNECT_STM_CHUNK_SIZE
 */
esp_err_t connect_stm_ota_data(uint16_t seq, const uint8_t *buf, uint16_t len);

/**
 * @brief 发送 OTA_FINISH 并等待 STM32 的 CRC_RESULT
 */
esp_err_t connect_stm_ota_finish(uint32_t crc32);

/**
 * @brief 通知 STM32 中止本次 OTA
 */
esp_err_t connect_stm_ota_abort(connect_stm_abort_reason_t reason);

/**
 * @brief 发送 LINK_STATUS 帧（STM32 侧据此刷新屏幕状态，无需 ACK）
 */
esp_err_t connect_stm_send_link_status(const connect_stm_link_status_t *st);

/**
 * @brief 最近 CONFIG_CONNECT_STM_LINK_TIMEOUT_MS 内是否收到过 STM32 的帧
 */
bool connect_stm_is_link_up(void);

#ifdef __cplusplus
}
#endif
