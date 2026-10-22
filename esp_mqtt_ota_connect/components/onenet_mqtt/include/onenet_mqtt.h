/*
 * onenet_mqtt —— OneNET 旧版多协议接入 MQTT 连接与收发管理
 *
 * 职责：
 *   1. 用三元组生成 token 并建立 MQTT(S) 连接，断线自动重连
 *   2. 订阅属性设置等下行 topic，解析后通过回调交给上层
 *   3. 提供属性上报、OTA 进度上报接口
 *
 * 本组件不关心业务：收到任何下行 JSON 都原样回调，由 main.c 决定如何处理。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 收到下行消息时的回调
 *
 * @param topic 消息所属 topic（完整字符串）
 * @param json  原始 payload，不保证以 '\0' 结尾
 * @param len   payload 长度
 *
 * 注意：在 MQTT 事件任务上下文中回调，请勿阻塞。
 */
typedef void (*onenet_downlink_cb_t)(const char *topic, const char *json, int len);

/**
 * @brief 初始化（仅创建资源，不联网）
 */
esp_err_t onenet_mqtt_init(void);

/**
 * @brief 生成 token 并启动 MQTT 客户端
 *
 * 前置条件：WiFi 已连接、SNTP 已同步（token 的 et 依赖系统时间）。
 */
esp_err_t onenet_mqtt_start(void);

/**
 * @brief 重新生成 token（长时间运行后 token 过期时使用）
 */
esp_err_t onenet_mqtt_refresh_token(void);

/**
 * @brief 注册下行消息回调，传 NULL 取消注册
 */
void onenet_mqtt_register_downlink_cb(onenet_downlink_cb_t cb);

/**
 * @brief 上报属性
 *
 * @param params_json 形如 {"temperature":26.5,"wifi_rssi":-55} 的 JSON 对象
 */
esp_err_t onenet_mqtt_publish_property(const char *params_json);

/**
 * @brief 上报 OTA 升级进度
 *
 * @param stage   阶段名，如 "downloading" / "verifying" / "success" / "failed"
 * @param percent 进度百分比，0~100；不适用时传 -1
 * @param msg     附加说明，可为 NULL
 */
esp_err_t onenet_mqtt_report_ota_progress(const char *stage, int percent, const char *msg);

/**
 * @brief MQTT 是否已连接
 */
bool onenet_mqtt_is_connected(void);

/**
 * @brief 发布一条原始消息到指定 topic
 */
esp_err_t onenet_mqtt_publish_raw(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif
