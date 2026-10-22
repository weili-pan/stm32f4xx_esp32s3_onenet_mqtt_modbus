/*
 * get_wifi_ota —— WiFi 连接 + OTA 固件下载
 *
 * 职责：
 *   1. 连接 WiFi(STA) 并断线自动重连，提供连通性查询
 *   2. 按 OTA 任务描述从 HTTP(S) 流式下载固件
 *      - 目标是 STM32：不落 ESP32S3 flash，边下边通过 UART 分片转发给 STM32
 *      - 目标是 ESP32S3：写入 OTA 分区并切换启动分区，重启后生效
 *   3. 通过进度回调外抛下载进度与结果
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_TARGET_STM32 = 0,
    OTA_TARGET_ESP32S3 = 1,
} ota_target_t;

typedef struct {
    ota_target_t target;
    char url[256];      /* 固件下载地址 */
    char version[32];   /* 目标版本号，会随 OTA_START 发给 STM32 */
    char md5[33];       /* 期望的 MD5 十六进制串，可为空表示不校验 */
    uint32_t size;      /* 期望的总字节数，0 表示以 HTTP Content-Length 为准 */
} ota_job_t;

/**
 * 下载过程回调
 *
 * @param job     当前任务
 * @param done    已下载字节数
 * @param total   总字节数，未知时为 0
 * @param err     ESP_OK 表示进行中/成功；其它值表示失败，此时为最后一次回调
 * @param finished true 表示任务已结束（成功或失败都置 true）
 */
typedef void (*ota_progress_cb_t)(const ota_job_t *job, uint32_t done, uint32_t total,
                                 esp_err_t err, bool finished);

/**
 * @brief 初始化 WiFi(STA) 并开始自动重连
 */
esp_err_t get_wifi_ota_init(void);

/**
 * @brief 阻塞等待 WiFi 获取到 IP
 *
 * @param timeout_ms 超时时间
 * @return true 已连接并拿到 IP
 */
bool wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief WiFi 是否已连接并拿到 IP
 */
bool wifi_is_connected(void);

/**
 * @brief 当前 WiFi 信号强度 (dBm)，未连接时返回 0
 */
int8_t wifi_get_rssi(void);

/**
 * @brief 投递一个 OTA 任务，立即返回，由内部任务异步执行
 *
 * @return ESP_ERR_INVALID_STATE 已有任务在执行
 */
esp_err_t get_wifi_ota_start(const ota_job_t *job);

/**
 * @brief 注册进度回调
 */
void get_wifi_ota_register_progress_cb(ota_progress_cb_t cb);

/**
 * @brief 是否有 OTA 任务正在执行
 */
bool get_wifi_ota_is_busy(void);

#ifdef __cplusplus
}
#endif
