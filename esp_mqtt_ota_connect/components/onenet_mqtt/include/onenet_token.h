/*
 * OneNET 旧版多协议接入 —— MQTT 密码(Token) 生成
 *
 * 算法（version = 2018-10-31）：
 *   stringForSignature = et + "\n" + method + "\n" + res + "\n" + version
 *   sign    = base64( hmac_<method>( base64_decode(accessKey), stringForSignature ) )
 *   token   = "version=..&res=..&et=..&method=..&sign=.."   (各 value 做 URL 编码)
 *
 * MQTT 连接映射：ClientID = DeviceName，Username = ProductKey(产品ID)，Password = token
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONENET_TOKEN_VERSION "2018-10-31"

/**
 * @brief 生成 OneNET 设备级接入 token
 *
 * @param out         输出缓冲
 * @param out_size    输出缓冲大小，建议 >= 256
 * @param product_key 产品 ID (ProductKey)
 * @param device_name 设备名称 (DeviceName)
 * @param device_secret 设备密钥 (DeviceSecret)，base64 字符串
 * @param et          过期时间，Unix 时间戳（秒）。传 0 表示使用当前时间 + 默认有效期
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_SIZE 缓冲不足
 */
esp_err_t onenet_token_generate(char *out, size_t out_size, const char *product_key,
                                const char *device_name, const char *device_secret, uint32_t et);

/**
 * @brief 仅计算签名字符串的签名结果（base64），便于与 OneNET 官方工具比对
 *
 * 注意：内部使用 system 时间，因此调用前需确保 SNTP 已同步。
 */
esp_err_t onenet_token_sign(char *out, size_t out_size, const char *resource, uint32_t et,
                            const char *method, const char *access_key);

#ifdef __cplusplus
}
#endif
