/**
 * @file esp_link.h
 * @brief ESP32S3 -> STM32 的只读状态链路（走 USART1）。
 *
 * 帧格式与 ESP32 侧 components/connect_stm 逐字节一致：
 *   AA 55 | TYPE(1) | SEQ(2 LE) | LEN(2 LE) | PAYLOAD | CRC16(2 LE)
 *   CRC16/MODBUS（poly 0xA001 反射），覆盖 TYPE..PAYLOAD，不含 SOF。
 *
 * 本工程不擦写内部 Flash，所以接收用普通字节中断 + 环形缓冲即可
 * （stm_ota 那边因为在收帧时擦 Flash、中断会被阻塞，才必须上 DMA）。
 */
#ifndef __ESP_LINK_H
#define __ESP_LINK_H

#include "main.h"

#include <stdbool.h>

/*----------------------------------------------- 帧结构 ----------------------------------------------*/

#define ESP_LINK_SOF_0          0xAAU
#define ESP_LINK_SOF_1          0x55U
#define ESP_LINK_HEADER_LEN     7U      /* SOF(2) + TYPE(1) + SEQ(2) + LEN(2) */
#define ESP_LINK_CRC_LEN        2U

/* 帧类型段约定（见 connect_stm.h）：0x01-0x0F = ESP32 -> STM32。
 * 现有 0x01-0x04 是 OTA，这里新增一个只读状态帧。 */
#define ESP_LINK_FRAME_LINK_STATUS  0x05U   /* payload = esp_link_status_t */

#define ESP_LINK_RX_RING_SIZE   512U    /* 必须是 2 的幂 */
#define ESP_LINK_MAX_PAYLOAD    64U     /* 状态帧只有 5 字节，留点余量 */
#define ESP_LINK_LINK_TIMEOUT_MS 3000U  /* 超过这么久没收到有效帧就判链路掉线 */

/*----------------------------------------------- 状态 payload ---------------------------------------*/

/* 0x05 帧的 payload（packed，小端，共 5 字节），与 ESP32 侧定义一致 */
typedef struct __attribute__((packed)) {
    uint8_t wifi;       /* 1 = WiFi 已连接 */
    uint8_t mqtt;       /* 1 = OneNET MQTT 已连接 */
    int8_t  rssi;       /* dBm，如 -55 */
    uint8_t devices;    /* 在线的 Modbus 从站数 */
    uint8_t ota_ready;  /* 1 = OTA 就绪 */
} esp_link_status_t;

/*----------------------------------------------- API -------------------------------------------------*/

/** 在 USART1（CubeMX 已配好 huart1）上挂字节中断接收 + 帧解析。 */
void EspLink_Init(void);

/** 主循环里反复调用：搬字节、跑帧解析状态机。 */
void EspLink_Poll(void);

/**
 * @brief 取最新状态。
 * @param out  非 NULL 时填入最近一帧 0x05 的状态（从没收到过则清零）
 * @return 链路是否在线（距上次有效帧 < ESP_LINK_LINK_TIMEOUT_MS）
 */
bool EspLink_GetStatus(esp_link_status_t *out);

#endif /* __ESP_LINK_H */
