/**
 * @file ui_env_monitor.h
 * @brief 环境监测主界面（竖屏 240x320，ST7789）。
 *
 * 布局参照 download.png，按竖屏重新排：顶部标题栏 -> 两张传感器卡片上下堆叠
 * -> 底部状态条。本文件不依赖任何模拟器/SDL，可以直接搬到 stm_modbus 里用。
 */
#ifndef UI_ENV_MONITOR_H
#define UI_ENV_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 建出整个界面（在当前活动屏幕上）。 */
void ui_env_monitor_create(void);

/**
 * 更新某张卡片的读数。
 * @param idx   0 = 传感器 01，1 = 传感器 02
 * @param temp  温度字符串，如 "26.5"（可以为 NULL 表示 "--"）
 * @param humi  湿度字符串，如 "58"
 */
void ui_env_set_values(uint8_t idx, const char *temp, const char *humi);

/** 顶部 WiFi 图标 + 「已连接 / 未连接」的绿/灰状态。 */
void ui_env_set_link(bool connected);

/** 底部「OTA 就绪」小圆点的亮/灭。 */
void ui_env_set_ota_ready(bool ready);

/** 底部「设备在线 / 设备离线」：在线从站数 >0 显示在线（绿），否则离线（灰）。 */
void ui_env_set_device_online(uint8_t count);

/** 底部「WiFi -55 dBm」：valid=0 时显示成「WiFi -- dBm」。 */
void ui_env_set_rssi(bool valid, int dbm);

#ifdef __cplusplus
}
#endif

#endif /* UI_ENV_MONITOR_H */
